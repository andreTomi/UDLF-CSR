#!/usr/bin/env bash
# step3b_sort_ab.sh -- before/after measurement for the finalize_candidates sort.
# Usage: bash step3b_sort_ab.sh <census-binary> <label>
# Sequential by construction: the runs would otherwise compete for the very
# cores whose contention is being attributed.
set -u
BIN=${1:?census binary}
LBL=${2:-run}
OUT=/tmp/sort_ab_${LBL}.txt
: > "$OUT"
for T in 1 4 12; do
  echo "===== $LBL T=$T" >> "$OUT"
  OMP_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE RFE_FULL=1 RFE_F6PROF=1 \
    "$BIN" /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin 400 10 2 0.1 2>&1 \
    | grep -v BuildV2 \
    | grep -E 't_total|t_disc|t_sort|t_acc|t_emit|cand_raw|raw/uniq|ck_gram|RKPIPEFULL|RKF7B' \
    >> "$OUT"
done
cat "$OUT"
