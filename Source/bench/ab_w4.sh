#!/bin/bash
# w4 probe: HASH branch with an L1-busting table (512 KB), the only regime
# where contiguous-pair materialization could plausibly pay off.
set -e
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I /tmp/inc_f1 -I Source/src \
    Source/bench/bench_phase.cpp -o /tmp/bp_ab_f1 -ltbb
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src \
    Source/bench/bench_phase.cpp -o /tmp/bp_ab_f3b -ltbb
echo "=== BUILD OK ==="
echo "=== params w4 ==="
/tmp/bp_ab_f1 params w4
for v in f1 f3b; do
  echo "=== w4 $v ==="
  taskset -c 0 env PHASE_THREADS=1 OMP_NUM_THREADS=1 PHASE_MEM=1 \
    /tmp/bp_ab_$v mult w4 3 > /tmp/ab/${v}_w4.txt 2>&1
  grep -E '^REP,|F1_ACCUM,|F2_PRUNE,|MP_CALL,|DONE,' /tmp/ab/${v}_w4.txt
  grep '^PHASEPROFILE,CTX' /tmp/ab/${v}_w4.txt
done
echo ALL DONE
