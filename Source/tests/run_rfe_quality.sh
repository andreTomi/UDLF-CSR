#!/usr/bin/env bash
# run_rfe_quality.sh -- Fase 0: baseline quality + time + RSS sweep.
#
# Arms per config: (a) input ranking, (b) legacy RFERef (bit-exact oracle),
# (c) exact RFECore. (b) and (c) MUST agree on ck_rk; the final rank lists are
# evaluated with rfe_quality.py (MAP/P@n/Recall@n, n in {5,10,20,50} + MAP@L).
#
# Datasets: corel5k (N=5000), linnaeus (N=8000), aloi (N=72000, optional).
# Configs:  k in {5,10,20} x L in {400,1000}, t=2, pa=0.1, plus ONE CC round
#           (k=20, L=400, th_cc=0) per dataset where memory allows.
#
# ALOI LEGACY WARN: RFERef materialises the dense NxN matrix -> 20.74 GB at
# N=72000. This host has 16 GB total / ~7 GB in WSL: the legacy arm on ALOI is
# REFUSED (task requires >= 32 GB). Recorded as AVISO in the summary; the
# baseline for ALOI is (a) input + (c) exact only.
# ALOI CC: F8g's A_Dense matrix is also NxN -> 20.74 GB, so the CC round on
# ALOI is refused too (the CSR emission arrives in Fase 3).
#
# Resume: each run is skipped if its log already contains "RFEQ wrote=".
# Usage (WSL, repo root):
#   bash Source/tests/run_rfe_quality.sh [all|corel5k|linnaeus|aloi|bridge]
#
# Knobs: T (threads, default 12), REPS_DET (determinism reps, default 2)
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
T=${T:-12}
REPS_DET=${REPS_DET:-2}
OUT="$ROOT/Data/phase_profile/rfe_fast/raw"
WORK=/tmp/rfeq
BIN=/tmp/bench_rfe_quality
export OMP_WAIT_POLICY=PASSIVE
export OMP_NUM_THREADS=$T

mkdir -p "$OUT" "$WORK" || exit 1
fail() { echo "F0 FAILED: $*" >&2; exit 1; }

# --------------------------------------------------------------- build ------
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp \
    -I Source/src -I Source/src/Core/hpp -I Source/tests \
    Source/bench/bench_rfe_quality.cpp -o "$BIN" \
    || fail "build bench_rfe_quality"

SUM="$OUT/summary.csv"
[ -f "$SUM" ] || echo "dataset,L,k,cc,arm,wall_s,rss_kb,ck_rk,log" > "$SUM"

# ---------------------------------------------------------- data prep --------
# rk file per dataset/L: text (self at index 0, EXACTLY L columns per row).
# Derived files are (re)built with mk_rk_from_leading_id.py (provenance) or a
# prefix truncation (ALOI rkLists.txt already has self at index 0).
prepare_rk() {  # ds L -> echoes rk path, exit 3 if unavailable
  local ds=$1 L=$2
  case "$ds" in
  corel5k)
    local f="/tmp/rk_real/corel5k_L${L}.txt"
    if [ ! -f "$f" ]; then
      python3 Source/tests/mk_rk_from_leading_id.py Datasets/corel5k/ACC.txt \
        "$L" "$f" "corel5k_L${L}" > "$OUT/mk_rk_corel5k_L${L}.log" 2>&1 \
        || { echo "  (derivation refused, see log)"; return 3; }
    fi
    echo "$f" ;;
  linnaeus)
    local f="/tmp/rk_real/linnaeus_L${L}.txt"
    if [ ! -f "$f" ]; then
      python3 Source/tests/mk_rk_from_leading_id.py \
        Datasets/linnaeus/linnaeus5_resnet50.txt "$L" "$f" "linnaeus_L${L}" \
        > "$OUT/mk_rk_linnaeus_L${L}.log" 2>&1 \
        || { echo "  (derivation refused, see log)"; return 3; }
    fi
    echo "$f" ;;
  aloi)
    # rkLists.txt: N=72000 rows x 7200 cols, self at index 0 -> prefix to L.
    local f="$WORK/aloi_L${L}.txt"
    if [ ! -f "$f" ]; then
      echo "  deriving aloi_L${L} (prefix truncation of 3 GB rkLists.txt)..." >&2
      awk -v L="$L" '{ for (i = 1; i <= L; i++) printf "%s%s", $i, (i < L ? " " : "\n") }' \
          Datasets/aloi/rkLists.txt > "$f.tmp" || return 3
      mv "$f.tmp" "$f"   # atomic: a reboot mid-write leaves .tmp, not a short $f
      {
        echo "DERIVED dataset"
        echo "label      = aloi_L${L}"
        echo "source     = Datasets/aloi/rkLists.txt"
        echo "N          = 72000"
        echo "L          = $L  (prefix truncation; self already at index 0)"
        echo "transform  = keep first L tokens of each row (RFE input width L=$L)"
        echo "output_md5 = $(md5sum "$f" | cut -d' ' -f1)"
      } > "$f.provenance"
    fi
    echo "$f" ;;
  *) return 3 ;;
  esac
}

# class file per dataset (Evaluation's class-id mapping source).
classes_of() {
  case "$1" in
  corel5k)  echo "Datasets/corel5k/corel5k_classes.txt" ;;
  linnaeus) echo "Datasets/linnaeus/classes_linnaeus_complete.txt" ;;
  aloi)     echo "Datasets/aloi/classes.txt" ;;
  esac
}
n_of() { case "$1" in corel5k) echo 5000;; linnaeus) echo 8000;; aloi) echo 72000;; esac; }

# --------------------------------------------------------------- runner ----
# run_arm <ds> <N> <L> <k> <cc> <arm> <rk> <classes>
#   Runs one arm, writes log + raw u32 sink + metrics into $OUT. Resume by
#   content (the "RFEQ wrote=" line is the bench's last act).
run_arm() {
  local ds=$1 N=$2 L=$3 k=$4 cc=$5 arm=$6 rk=$7 cls=$8
  local tag="${ds}_L${L}_k${k}"; [ "$cc" = 1 ] && tag="${tag}_cc"
  local log="$OUT/run_${tag}_${arm}.log"
  local out="$WORK/${tag}_${arm}.u32"

  if grep -q 'RFEQ wrote=' "$log" 2>/dev/null; then
    echo "  $tag/$arm: resume-skip (verdict present)"
    return 0
  fi

  if [ "$arm" = legacy ] && [ "$N" -ge 72000 ]; then
    {
      echo "RFEQ arm=legacy REFUSED memory"
      echo "AVISO: legacy NxN dense at N=$N needs 20.74 GB; host has 16 GB total"
      echo "       (WSL ~7 GB). Task requires a machine with >=32 GB for ALOI legacy."
    } | tee "$log"
    return 0
  fi

  # exact + CC on ALOI: F8g's A_Dense variant materialises an NxN float matrix
  # (RFECore.hpp emb_by_cc_dense_ / f8g_matrix_dense_, both n*n) -> 20.74 GB at
  # N=72000. Same refusal as legacy; the CSR emission (Fase 3) is what lifts it.
  if [ "$arm" = exact ] && [ "$cc" = 1 ] && [ "$N" -ge 72000 ]; then
    {
      echo "RFEQ arm=exact cc=1 REFUSED memory"
      echo "AVISO: F8g A_Dense NxN at N=$N needs 20.74 GB (same as legacy CC)."
      echo "       Refused until Fase 3 emits CSR directly from cc_for_k1."
    } | tee "$log"
    return 0
  fi

  local binflag=() ; case "$rk" in *.u32|*.bin) binflag=(--bin) ;; esac
  local ccflag=() ; [ "$cc" = 1 ] && ccflag=(--cc --th_cc 0)
  local threads=() ; [ "$arm" != legacy ] && [ "$arm" != input ] && threads=(--threads "$T")

  echo "  $tag/$arm: running..."
  local t0=$(date +%s.%N)
  RFE_ALLOW_CC=1 "$BIN" "$arm" "$rk" "$N" "$L" "$k" 2 0.1 "$out" \
      "${binflag[@]}" "${ccflag[@]}" "${threads[@]}" > "$log" 2>&1
  local rc=$?
  local dt
  dt=$(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN{printf "%.3f", a-b}')
  if [ $rc -ne 0 ]; then
    echo "  $tag/$arm: FAILED rc=$rc (see $(basename "$log"))"
    return $rc
  fi
  # wrapper metrics: wall (driver), RSS+wall+ck (bench), MAP/P/Recall (python)
  {
    echo "RFEQ driver_wall_s=$dt"
    python3 Source/bench/rfe_quality.py metrics --rk "$out" --bin --L "$L" \
      --classes "$cls" --ks 5,10,20,50 --map-ks 5,10,20,50,"$L" 2>&1
  } >> "$log" 2>&1
  # summary row
  local wall rss ck
  wall=$(grep -o 'wall_s=[0-9.]*' "$log" | head -1 | cut -d= -f2)
  rss=$(grep -o 'VmHWM_kb=[0-9]*' "$log" | head -1 | cut -d= -f2)
  ck=$(grep -o 'ck_rk=0x[0-9a-f]*' "$log" | head -1 | cut -d= -f2)
  echo "$ds,$L,$k,$cc,$arm,$wall,$rss,$ck,run_${tag}_${arm}.log" >> "$SUM"
  echo "  $tag/$arm: wall=${wall}s rss=${rss}kB ck=$ck"
}

# ---------------------------------------------------------------- checks ----
# parity: legacy ck MUST equal exact ck (bit-identical baseline (b)==(c))
check_parity() {  # ds L k cc
  local tag="$1_L$2_k$3"; [ "$4" = 1 ] && tag="${tag}_cc"
  local fl="$OUT/run_${tag}_legacy.log" fe="$OUT/run_${tag}_exact.log"
  local cl ce
  cl=$(grep -o 'ck_rk=0x[0-9a-f]*' "$fl" 2>/dev/null | head -1 | cut -d= -f2)
  ce=$(grep -o 'ck_rk=0x[0-9a-f]*' "$fe" 2>/dev/null | head -1 | cut -d= -f2)
  if [ -z "$cl" ] || [ -z "$ce" ]; then
    echo "PARITY $tag: NOT-EVALUATED (legacy='${cl:-none}' exact='${ce:-none}')"
    echo "PARITY,$tag,${cl:-none},${ce:-none},NOT-EVALUATED" >> "$OUT/parity.csv"
    return 0
  fi
  if [ "$cl" = "$ce" ]; then
    echo "PARITY $tag: OK ($cl)"
    echo "PARITY,$tag,$cl,$ce,OK" >> "$OUT/parity.csv"
    return 0
  fi
  echo "PARITY $tag: FAIL legacy=$cl exact=$ce"
  echo "PARITY,$tag,$cl,$ce,FAIL" >> "$OUT/parity.csv"
  return 1
}

# determinism: R reps of the exact arm must produce identical ck_rk
# (acceptance: deterministic between executions with the SAME thread count)
check_determinism() {  # ds N L k rk classes
  local ds=$1 N=$2 L=$3 k=$4 rk=$5 cls=$6
  local tag="${ds}_L${L}_k${k}_det" ck_ref="" r
  for r in $(seq 1 "$REPS_DET"); do
    local log="$OUT/run_${tag}_rep${r}.log"
    if ! grep -q 'RFEQ wrote=' "$log" 2>/dev/null; then
      RFE_ALLOW_CC=1 "$BIN" exact "$rk" "$N" "$L" "$k" 2 0.1 \
        "$WORK/${tag}_rep${r}.u32" --threads "$T" > "$log" 2>&1 || \
        { echo "DET $tag rep$r FAILED (see $(basename "$log"))"; return 1; }
    fi
    local ck
    ck=$(grep -o 'ck_rk=0x[0-9a-f]*' "$log" | head -1 | cut -d= -f2)
    rm -f "$WORK/${tag}_rep${r}.u32"   # tmpfs budget: sink read, digest kept
    if [ -z "$ck_ref" ]; then ck_ref=$ck
    elif [ "$ck" != "$ck_ref" ]; then
      echo "DET $tag: FAIL rep$r=$ck != rep1=$ck_ref"
      echo "DET,$tag,$ck_ref,$ck,FAIL" >> "$OUT/determinism.csv"
      return 1
    fi
  done
  echo "DET $tag: OK ($ck_ref x $REPS_DET reps)"
  echo "DET,$tag,$ck_ref,$ck_ref,OK" >> "$OUT/determinism.csv"
}

# ------------------------------------------------------------ main sweep ----
# Arms: (a) input, (b) legacy, (c) exact. One CC round per dataset (k=20,
# th_cc=0) as required by the task.
sweep_dataset() {  # ds
  local ds=$1
  local N; N=$(n_of "$ds")
  local cls; cls=$(classes_of "$ds")
  [ -f "$cls" ] || fail "classes file missing: $cls"
  echo "=== dataset=$ds N=$N classes=$cls ==="
  for L in 400 1000; do
    local rk
    rk=$(prepare_rk "$ds" "$L") || {
      echo "  L=$L: rk derivation unavailable -> SKIP (log: $OUT/mk_rk_*.log)"
      continue; }
    for k in 5 10 20; do
      run_arm "$ds" "$N" "$L" "$k" 0 input  "$rk" "$cls"
      run_arm "$ds" "$N" "$L" "$k" 0 legacy "$rk" "$cls"
      run_arm "$ds" "$N" "$L" "$k" 0 exact  "$rk" "$cls"
      check_parity "$ds" "$L" "$k" 0 || true
      check_determinism "$ds" "$N" "$L" "$k" "$rk" "$cls" || true
    done
    # the single CC round (legacy runs exec_rr_ccs; exact needs RFE_ALLOW_CC)
    run_arm "$ds" "$N" "$L" 20 1 input  "$rk" "$cls"
    run_arm "$ds" "$N" "$L" 20 1 legacy "$rk" "$cls"
    run_arm "$ds" "$N" "$L" 20 1 exact  "$rk" "$cls"
    check_parity "$ds" "$L" 20 1 || true
  done
}

# ---------------------------------------------------------------- bridge -----
# ONE execution of the REAL evaluation module vs rfe_quality.py (task Fase 0).
run_bridge() {
  echo "=== bridge: Evaluation.hpp (real module) vs rfe_quality.py ==="
  local g=/tmp/eval_bridge
  g++ -std=gnu++2b -O2 -fopenmp -I Source/src -I Source/src/Config/hpp \
      -I Source/src/Core/hpp -I Source/src/Utils/hpp -I Source/src/Evaluation/hpp \
      Source/tests/eval_bridge.cpp -o "$g" || fail "build eval_bridge"
  local cls=Datasets/corel5k/corel5k_classes.txt
  local rk; rk=$(prepare_rk corel5k 400) || fail "corel5k L400 rk"
  # produce both arms as u32 (resume-safe)
  grep -q 'RFEQ wrote=' "$OUT/bridge_input.log" 2>/dev/null || \
    "$BIN" input "$rk" 5000 400 10 2 0.1 "$WORK/bridge_in.u32" \
      > "$OUT/bridge_input.log" 2>&1 || fail "bridge input arm"
  grep -q 'RFEQ wrote=' "$OUT/bridge_legacy.log" 2>/dev/null || \
    "$BIN" legacy "$rk" 5000 400 10 2 0.1 "$WORK/bridge_leg.u32" \
      > "$OUT/bridge_legacy.log" 2>&1 || fail "bridge legacy arm"
  # u32 -> text (Evaluation reads text via assignFromTextFile)
  python3 - "$WORK" <<'PYEOF'
import sys, numpy as np, os
w = sys.argv[1]
for name in ('bridge_in', 'bridge_leg'):
    a = np.fromfile(os.path.join(w, name + '.u32'), dtype=np.uint32)
    assert a.size == 5000 * 400, (name, a.size)
    with open(os.path.join(w, name + '.txt'), 'w') as f:
        for row in a.reshape(5000, 400):
            f.write(' '.join(map(str, row)) + '\n')
PYEOF
  "$g" "$cls" "$WORK/bridge_in.txt" "$WORK/bridge_leg.txt" \
       "$OUT/bridge_eval.log" 5000 400 > "$OUT/bridge_run.log" 2>&1 \
    || fail "eval_bridge execution (see $OUT/bridge_run.log)"
  python3 Source/bench/eval_bridge_check.py \
    --log "$OUT/bridge_eval.log" --in "$WORK/bridge_in.txt" \
    --leg "$WORK/bridge_leg.txt" --classes "$cls" --L 400 \
    | tee "$OUT/bridge_check.txt" || fail "bridge check FAILED"
}

# ------------------------------------------------------------------ main ----
case "${1:-all}" in
  bridge)   run_bridge ;;
  corel5k)  run_bridge; sweep_dataset corel5k ;;
  linnaeus) run_bridge; sweep_dataset linnaeus ;;
  aloi)     sweep_dataset aloi ;;
  all)      run_bridge; sweep_dataset corel5k; sweep_dataset linnaeus
            sweep_dataset aloi ;;
  *) echo "usage: $0 [all|corel5k|linnaeus|aloi|bridge]" >&2; exit 2 ;;
esac

echo
echo "=== Fase 0 summary ==="
echo "--- summary.csv ---"; cat "$SUM"
echo "--- parity.csv ---"; cat "$OUT/parity.csv" 2>/dev/null || echo "(none)"
echo "--- determinism.csv ---"; cat "$OUT/determinism.csv" 2>/dev/null || echo "(none)"
echo "Artifacts: $OUT"


