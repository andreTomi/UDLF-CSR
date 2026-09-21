#!/usr/bin/env bash
# _decisive.sh -- decisive test: were the old "c0-5"/"call" points measured on
# 6/12 CPUs, or silently restricted to the 2 CPUs of core 0?
#
# Same thread count, two masks: a 2-CPU mask must be dramatically slower than
# the full mask if the mask was honored. Compare with the recorded medians.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
BIN=/tmp/bench_a_base

one() { # one <mask> <threads> <reps> <wl>
  local mask=$1 th=$2 reps=$3 wl=$4 med
  med=$(taskset -c "$mask" env PHASE_THREADS=$th OMP_NUM_THREADS=$th \
        OMP_PROC_BIND=true OMP_PLACES=cores "$BIN" mult "$wl" "$reps" 2>/dev/null \
        | grep '^REP' | tail -n +2 | awk -F, '{print $3}' | sort -n \
        | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')
  printf '  mask=%-6s threads=%-3s -> %8s ms\n' "$mask" "$th" "$med"
}

echo "### w1, 12 threads, 5 reps  (recorded curve_w1_call_t12 = 138.8 ms)"
one 0,1 12 5 w1
one 0-1 12 5 w1
one all 12 5 w1
echo "### w1, 6 threads, 5 reps   (recorded curve_w1_c0-5_t6 = 162.8 ms)"
one 0,1 6 5 w1
one 0-5 6 5 w1
echo "### w1, 4 threads, 5 reps   (recorded curve_w1_c0-3_t4 = 193.4 ms)"
one 0,1 4 5 w1
one 0-3 4 5 w1
echo "### w3, 6 threads, 3 reps   (recorded curve_w3_c0-5_t6 = 4550.4 ms)"
one 0,1 6 3 w3
one 0-5 6 3 w3
echo "### w3, 12 threads, 3 reps  (recorded curve_w3_call_t12 = 4520.6 ms)"
one 0,1 12 3 w3
one all 12 3 w3
