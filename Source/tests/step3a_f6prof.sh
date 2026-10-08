#!/bin/bash
# Step 3 / Stage 3a: F6 cost attribution on REAL ALOI.
# Sequential by construction -- running the sweep in parallel would have the
# runs compete for cores and destroy the very timing being attributed.
set -u
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
OUT=${1:-/tmp/f6prof.txt}
: > "$OUT"
for T in 1 2 4 8 12; do
  echo "===== threads=$T =====" >> "$OUT"
  OMP_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE RFE_FULL=1 RFE_F6PROF=1 \
    /tmp/bench_rfe_census "$BIN" 400 10 2 0.1 2>&1 \
    | grep -v BuildV2 \
    | grep -E 'RKF6|RKF6PROF|t_total|phase|t_disc|t_sort|t_acc|t_emit|work:|raw/uniq|est insert|RKPIPEFULL|RKF7B' \
    >> "$OUT"
done
cat "$OUT"