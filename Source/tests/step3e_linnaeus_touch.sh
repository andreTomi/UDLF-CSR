#!/usr/bin/env bash
# step3e_linnaeus_touch.sh -- touch-first census on linnaeus THROUGH run().
#
# Why this exists: bench_rfe_census was the instrument that spelled the sequence
# itself and measured a pipeline with no t-loop (FINDINGS, X9). It now calls
# run(); this driver is the check that the FIXED census reproduces the gate's
# FAITHFUL pins on the same input -- the instrument validating against the gate.
#
# The census reads raw int32-LE binaries (load_raw derives N from the file
# size), so the derived linnaeus_L400.txt (same script + same md5 the gate and
# the sweep use) is converted here, inside this single invocation.
#
# Cross-checks against values committed BEFORE this run:
#   k=10 : RKF6  ck_gram == 0xa584ebbe38e86571  (gate's faithful pin)
#          RKF7B ck_rk   == 0x09b624c5caae064c  (gate's faithful pin)
#   k=20 : RKF7B ck_rk   == 0xcb530cc3a7006366  (sweep E2E = faithful)
#          k=20 ck_gram/nnz/rho are a NEW faithful measurement -- recorded, NOT
#          compared: the sweep's 18,733,040 / 0x2b0ebc6f… / rho 0.2927 are the
#          SHORT (F6-section) state, superseded (comparing against them would
#          pit faithful x short).
#   both : RKGRAM nnz_est == RKF6 gram_nnz (union census == CSR Gram nnz).
#
# No timing claim is made here beyond what the log prints; run it while nothing
# else heavy is active (this arc's rule: no timing measurement with another
# process running).
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-Data/phase_profile/rfe_real_st0}
BINOUT=${BINOUT:-/tmp/step3e}
WORK=${WORK:-/tmp/linnaeus_touch}
FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"
mkdir -p "$OUT" "$BINOUT" "$WORK" || exit 1
LOG="$OUT/linnaeus_census.log"
fail() { echo "STEP3E FAILED: $*" | tee -a "$LOG" >&2; exit 1; }

: > "$LOG"
{
  echo "=== step3e_linnaeus_touch.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
  echo "flags: $FLAGS  threads: 12  RFE_FULL=1 RFE_F1PROF=1  pa=0.1 t=2"
} >> "$LOG"

echo "[1/3] derivar linnaeus_L400.txt" | tee -a "$LOG"
python3 Source/tests/mk_rk_from_leading_id.py Datasets/linnaeus/linnaeus5_resnet50.txt \
        400 "$WORK/linnaeus_L400.txt" linnaeus_L400 2>&1 \
  | grep -e wrote -e REFUSED | tee -a "$LOG"
[ -f "$WORK/linnaeus_L400.txt" ] || fail "mk_rk nao produziu o arquivo"
md5=$(md5sum "$WORK/linnaeus_L400.txt" | cut -d' ' -f1)
echo "  md5=$md5 (esperado dde830919922f758424282efaec41613)" | tee -a "$LOG"
[ "$md5" = "dde830919922f758424282efaec41613" ] || fail "md5 do input derivado divergiu"

echo "[2/3] converter para .bin int32 LE" | tee -a "$LOG"
python3 - "$WORK/linnaeus_L400.txt" "$WORK/linnaeus_L400.bin" >> "$LOG" 2>&1 <<'PY' \
  || fail "conversao txt->bin (ver $LOG)"
import struct, sys
vals = []
with open(sys.argv[1]) as f:
    for line in f:
        vals.extend(int(x) for x in line.split())
with open(sys.argv[2], "wb") as g:
    g.write(struct.pack("<%di" % len(vals), *vals))
print("bin cells=%d (esperado 3200000 = 8000*400)" % len(vals))
assert len(vals) == 3200000, "N*L != 8000*400"
PY

echo "[3/3] census k=10 e k=20 (RFE_FULL: run() + leitura do Gram)" | tee -a "$LOG"
$CXX $FLAGS -I Source/src -I Source/src/Core/hpp \
     Source/bench/bench_rfe_census.cpp -o "$BINOUT/bench_rfe_census" 2>>"$LOG" \
  || fail "build bench_rfe_census (ver $LOG)"

run_k() {
  local k=$1
  OMP_NUM_THREADS=12 RFE_FULL=1 RFE_F1PROF=1 \
    "$BINOUT/bench_rfe_census" "$WORK/linnaeus_L400.bin" 400 "$k" 2 0.1 \
    > "$WORK/census_k${k}.log" 2>&1
  local rc=$?
  { echo "--- k=$k (rc=$rc) ---"; cat "$WORK/census_k${k}.log"; } >> "$LOG"
  # rc=5 is RKCONTRACT_REFUSED: a refusal IS the touch-first finding, and it is
  # recorded as a failure here so it cannot be mistaken for a passing census.
  [ "$rc" = 0 ] || fail "census k=$k rc=$rc (ver $LOG)"
}

assert_k() {
  local k=$1 gram_want="$2" rkrk_want="$3"   # gram_want may be the word NEW
  local L="$WORK/census_k${k}.log"
  local ck_g ck_r est nnz
  ck_g=$(grep -o 'ck_gram=0x[0-9a-f]*' "$L" | head -1 | cut -d= -f2)
  ck_r=$(grep -o 'ck_rk=0x[0-9a-f]*' "$L" | head -1 | cut -d= -f2)
  est=$(grep -o 'nnz_est=[0-9]*' "$L" | head -1 | cut -d= -f2)
  nnz=$(grep -o 'gram_nnz=[0-9]*' "$L" | head -1 | cut -d= -f2)
  echo "  k=$k ck_gram=$ck_g ck_rk=$ck_r nnz_est=$est gram_nnz=$nnz" | tee -a "$LOG"
  [ -n "$ck_r" ] || fail "k=$k: ck_rk ausente (bloco RFE_FULL nao rodou)"
  [ -n "$nnz" ] || fail "k=$k: RKF6 gram_nnz ausente"
  [ "$ck_r" = "$rkrk_want" ] \
    || fail "k=$k: ck_rk fiel $ck_r != esperado $rkrk_want (census divergiu do gate)"
  if [ "$gram_want" = NEW ]; then
    echo "  k=$k ck_gram/nnz/rho: NOVA medicao fiel (sem expectativa pre-existente -- registrar)" | tee -a "$LOG"
  else
    [ "$ck_g" = "$gram_want" ] \
      || fail "k=$k: ck_gram fiel $ck_g != pin do gate $gram_want"
  fi
  [ "$est" = "$nnz" ] \
    || fail "k=$k: RKGRAM nnz_est=$est != RKF6 gram_nnz=$nnz (uniao != CSR)"
}

run_k 10
run_k 20
assert_k 10 0xa584ebbe38e86571 0x09b624c5caae064c
assert_k 20 NEW                 0xcb530cc3a7006366

echo "STEP3E: OK -- census fiel reproduz os dois pins do gate em k=10 e o ck_rk" | tee -a "$LOG"
echo "        fiel em k=20; nnz_est == gram_nnz nos dois; ck_gram fiel de" | tee -a "$LOG"
echo "        linnaeus k=20 medido (novo -- registrar em FINDINGS)." | tee -a "$LOG"
