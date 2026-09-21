#!/bin/bash
# Noise control: re-run the SAME f1 binary to bound run-to-run variation.
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
for w in w1 w2; do
  taskset -c 0 env PHASE_THREADS=1 OMP_NUM_THREADS=1 PHASE_MEM=1 /tmp/bp_ab_f1 mult $w 5 > /tmp/ab/f1b_$w.txt 2>&1
  echo "DONE f1b $w"
done
taskset -c 0 env PHASE_THREADS=1 OMP_NUM_THREADS=1 PHASE_MEM=1 /tmp/bp_ab_f1 mult w3 3 > /tmp/ab/f1b_w3.txt 2>&1
echo "DONE f1b w3"
echo "ALL DONE"
