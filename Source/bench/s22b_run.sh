#!/bin/bash
# s22b_run.sh -- S2.2b same-session interleaved A/B (legacy vs v2) for the
# REAL pipeline (LHRR at T in {1,2,12}, CPRR spot-check layer-0 at T=12).
#
# Inherits the permanent protocol policy from s1_bis_run.sh:
#   (1) one throwaway warm-up run per builder before any measurement
#   (2) discard rep 0 of every block (first-touch of a fresh process)
#   plus a canary (mult w1 2, fixed load, T=12) before/after to size session
#   drift, and ABBA pairs (legacy,v2 | v2,legacy alternating) so a monotone
#   drift cancels inside each pair. REPRU lines are kept as mechanism
#   evidence for the rep-0 penalty.
#
# The builder switch is a RUNTIME knob (PHASE_BUILDER) in the single driver
# /home/andre/udlf_bin/bp_s22 -- unlike s1_bis there is no per-arm rebuild,
# so both arms run the exact same binary and headers.
#
# Outputs are resume-able (each step is skipped if its file exists):
#   <outdir>/raw/{warm,p*,canary,cprr_*}.txt with ABKNOBS/DRVAGG/REP/REPRU/
#   DONE(hwm) lines, aggregated later into the S2.2b README.
#
# CRLF: run through fix_crlf.py first (repo convention):
#   python3 Source/bench/fix_crlf.py Source/bench/s22b_run.sh   # -> /tmp
#   bash /tmp/s22b_run.sh <1|2|12|cprr> [outdir]
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
WHAT=${1:?usage: s22b_run.sh <1|2|12|cprr> [outdir]}
case "$WHAT" in
  1)    MASK=0 ;;
  2)    MASK=0-1 ;;
  12)   MASK=0-11 ;;
  cprr) MASK=0-11 ;;
  mem)  MASK=0-11 ;;
  *) echo "unsupported target: $WHAT (want 1, 2, 12, cprr or mem)"; exit 2 ;;
esac
case "$WHAT" in cprr) T=12;; *) T=$WHAT;; esac
OUT=${2:-$R/Data/phase_profile/etapa_b/s22b/t$T}
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
DRIVER=/home/andre/udlf_bin/bp_s22
N=72000; L=400; K=10; LIMIT=400; REPS=2
mkdir -p "$OUT/raw"
cd "$R"

# DONE-aware: a run killed mid-way leaves a partial file that must be re-run,
# so mere presence (-s) is not enough to skip it.
have() { grep -q '^DONE,' "$OUT/raw/$1.txt" 2>/dev/null; }

run() {  # run <builder legacy|v2> <tag>   -- LHRR, one arm of the A/B
  have "$2" && { echo "skip $2"; return; }
  taskset -c "$MASK" env OMP_NUM_THREADS=$T PHASE_THREADS=$T PHASE_BUILDER=$1 \
    "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/raw/$2.txt" 2>&1
  echo "ran $2"
}
run_cprr() {  # run_cprr <builder> <tag>  -- CPRR layer-0 spot-check (T=12)
  have "$2" && { echo "skip $2"; return; }
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 PHASE_BUILDER=$1 \
    PHASE_RK_OUT="$OUT/raw/$2.rk.bin" \
    "$DRIVER" real "$BIN" $N $L $K $LIMIT cprr $REPS > "$OUT/raw/$2.txt" 2>&1
  echo "ran $2"
}
canary() {  # canary <tag> -- fixed load at T=12, sizes session drift
  have "$1" && { echo "skip $1"; return; }
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
    "$DRIVER" mult w1 2 > "$OUT/raw/$1.txt" 2>&1
  echo "ran $1"
}

# ---- memory variant (PHASE_MEM=1): second ΔHWM convention ---------------
# clear_refs resets VmHWM before each build, so after_build_max_kb - 
# before_build_max_kb is the BUILD RAISE (isolated peak), while the DONE line
# of the timing captures stays the true process peak (first convention).
if [ "$WHAT" = "mem" ]; then
  echo "--- PHASE_MEM=1 captures (3 reps), 6 builds"
  for spec in "1:0" "2:0-1" "12:0-11"; do
    mt=${spec%%:*}; mm=${spec##*:}
    for bb in legacy v2; do
      tag=mem_t${mt}_${bb}
      have "$tag" && { echo "skip $tag"; continue; }
      taskset -c "$mm" env OMP_NUM_THREADS=$mt PHASE_THREADS=$mt \
        PHASE_BUILDER=$bb PHASE_MEM=1 \
        "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr 3 > "$OUT/raw/$tag.txt" 2>&1
      echo "ran $tag"
    done
  done
  echo "DONE -> $OUT"
  exit 0
fi

if [ "$WHAT" = "cprr" ]; then
  echo "--- CPRR spot-check (layer-0), T=12"
  run_cprr legacy cprr_legacy
  run_cprr v2 cprr_v2
  echo "DONE -> $OUT"
  exit 0
fi

echo "--- warm-up (discarded, one per builder, T=$T)"
run legacy warm_legacy
run v2 warm_v2
canary canary_0

echo "--- pair 1: legacy, v2"
run legacy p1_legacy
run v2 p2_v2
echo "--- pair 2: v2, legacy"
run v2 p3_v2
run legacy p4_legacy
echo "--- pair 3: legacy, v2"
run legacy p5_legacy
run v2 p6_v2

canary canary_1
echo "DONE -> $OUT"