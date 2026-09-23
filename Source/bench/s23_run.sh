#!/bin/bash
# s23_run.sh -- S2.3 step 1: fragment-CSR vs v2, SAME emitted base, per matrix.
# This is the entry gate the S2 plan (doc 13) sets for S2.3: MEASURE the
# fragment-CSR-vs-v2 decision on the same emitted base, per matrix (duplicate
# profiles differ: MM ~30.2% dupes, H ~k^2-ish, ...), BEFORE any removal of
# temp_triplets_ / buildCSRParallel / <execution>.
#
# Protocol (inherits the S2.1/S2.2b policy):
#   - one throwaway warm-up run per T;
#   - canary (mult w3, T=12, fixed load, >=1s per rep -- S1-bis lesson; the
#     inherited mult w1 was a regression at ~64-121ms) before and after the
#     measured run to
#     size session drift;
#   - PHASE_FRAGAB=1 makes each of the six LHRR builds (seq 1..6 = MM, H, S,
#     Srev, Stot, result) emit FRAGPROBE/FRAGAB/FRAGPREP/FRAGABCHECK lines:
#     3 interleaved samples per builder over the identical LIVE base, plus the
#     fragment-contract verdict and the structure/value check;
#   - PHASE_BUILDER=v2 is REQUIRED (v2 is the natural arm; legacy is already
#     beaten by S2.2b and out of scope here);
#   - reps = 3 (rep 0 discarded; 2 reps x 3 intra samples = 6 samples per
#     builder per matrix).
# While PHASE_FRAGAB is on, PHASEPROFILE ledger lines are contaminated by the
# extra measurement builds: this campaign reads only FRAG* + REP/REPRU/DONE.
#
# Outputs are resume-able: a step runs only if its file lacks a DONE, line.
# Repo convention (files are CRLF): run through fix_crlf.py first:
#   python3 Source/bench/fix_crlf.py Source/bench/s23_run.sh   # -> /tmp
#   bash /tmp/s23_run.sh <1|12> [outdir]
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
WHAT=${1:?usage: s23_run.sh <1|12> [outdir]}
case "$WHAT" in
  1)  MASK=0 ;;
  12) MASK=0-11 ;;
  *) echo "unsupported target: $WHAT (want 1 or 12)"; exit 2 ;;
esac
T=$WHAT
OUT=${2:-$R/Data/phase_profile/etapa_b/s23/t$T}
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
DRIVER=/home/andre/udlf_bin/bp_s23
N=72000; L=400; K=10; LIMIT=400; REPS=3
mkdir -p "$OUT/raw"
cd "$R"

have() { grep -q '^DONE,' "$OUT/raw/$1.txt" 2>/dev/null; }

run() {  # run <tag> -- one full LHRR A/B run at the stage's T
  have "$1" && { echo "skip $1"; return; }
  taskset -c "$MASK" env OMP_NUM_THREADS=$T PHASE_THREADS=$T \
    PHASE_BUILDER=v2 PHASE_FRAGAB=1 \
    "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/raw/$1.txt" 2>&1
  echo "ran $1"
}
canary() {  # canary <tag> -- fixed load at T=12, sizes session drift.
  # w3 (not w1): S1-bis set the floor at >=1s per rep; w1 measured only
  # ~64-121ms here, too short to bound a noisy session (S2.3 lesson).
  have "$1" && { echo "skip $1"; return; }
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
    "$DRIVER" mult w3 2 > "$OUT/raw/$1.txt" 2>&1
  echo "ran $1"
}

echo "--- warm (discarded, T=$T)"
run warm
canary canary_0
echo "--- measured (PHASE_FRAGAB=1, builder=v2, T=$T)"
run m1
canary canary_1
echo "DONE -> $OUT"