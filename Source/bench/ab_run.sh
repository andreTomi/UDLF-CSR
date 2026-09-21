#!/bin/bash
# A/B run: reps=5, single core (taskset -c 0), both profiles.
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
OUT=${1:-/tmp/ab}
mkdir -p "$OUT"
for v in base f1 f13; do
  for w in w1 w2 w3; do
    f="$OUT/${v}_${w}.txt"
    taskset -c 0 env PHASE_THREADS=1 OMP_NUM_THREADS=1 PHASE_MEM=1 \
      /tmp/bp_ab_$v mult "$w" 5 > "$f" 2>&1
    echo "DONE $v $w $(grep -c '^REP,' "$f") reps"
  done
done
echo "ALL DONE"
