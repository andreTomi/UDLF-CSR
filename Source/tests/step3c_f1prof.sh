#!/usr/bin/env bash
# F1 profile sweep. Sequential by construction.
set -u
BIN=${1:-/tmp/census_f1}
LBL=${2:-run}
OUT=/tmp/f1prof_${LBL}.txt
: > "$OUT"
for T in 1 4 12; do
  echo "===== $LBL T=$T" >> "$OUT"
  OMP_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE RFE_FULL=1 RFE_F1PROF=1 \
    "$BIN" /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin 400 10 2 0.1 2>&1 \
    | grep -v BuildV2 \
    | grep -E 'RKF1PROF|t_total|t_index|t_loop|t_cells_max|t_build|t_sortrows|cells=|RKPIPEFULL|ck_gram' \
    >> "$OUT"
done
cat "$OUT"
