#!/bin/bash
set -u
cd /mnt/c/Users/andre/Documents/phd/side_projects/UDLF-CSR/UDLF-CSR || exit 1
ALOI=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    Source/bench/bench_rfe_census.cpp -o /tmp/peak_bin 2>/dev/null || { echo BUILD_FAIL; exit 1; }
for T in 12 1; do
  echo "===== T=$T ====="
  OMP_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE RFE_FULL=1 RFE_F1PROF=1 \
    timeout 900 /tmp/peak_bin "$ALOI" 400 10 2 0.1 2>&1 | grep -v BuildV2 \
    | grep -E 'PEAK|after f1|after t-loop|after embRR|after f5|after f6|after f7b|RKPIPEFULL'
done
