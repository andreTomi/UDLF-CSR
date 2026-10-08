#!/usr/bin/env bash
# Repetitions, because the first two samples of this benchmark disagreed by 40%.
set -u
REPS=${1:-7}
for i in $(seq 1 "$REPS"); do
  OMP_NUM_THREADS=1 /tmp/posidx /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin 72000 400 \
    | grep -E 'V1 |V2 |speedup|identity'
  echo "--- rep $i"
done
