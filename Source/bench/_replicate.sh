#!/usr/bin/env bash
# _replicate.sh -- prove the recorded curve points are not mask-corrupted.
#
# Discovery: TSET's `affinity` field is printed *after* the matrices are built,
# and OMP_PROC_BIND=true + OMP_PLACES=cores re-pins the master thread to its
# place on the first parallel region -- so the field reports that core's CPUs
# (e.g. "0,1") instead of the taskset mask. The driver now also prints
# `affinity_prebind`, captured at process entry.
#
# Validation: re-run a few of the labelled points with the new binary and
#   (a) check affinity_prebind == the mask the label claims,
#   (b) check the median reproduces the recorded one within noise.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
OUT=Data/phase_profile/etapa_a/replication
mkdir -p "$OUT"

echo "=== rebuild drivers"
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp -o /tmp/bench_a_base -ltbb || exit 1
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/bench/bench_phase.cpp -o /tmp/bench_a_s1 -ltbb || exit 1
echo "ok"

run() { # run <tag> <mask> <threads> <args...>
  local tag=$1 mask=$2 th=$3; shift 3
  local f="$OUT/$tag.txt"
  taskset -c "$mask" env PHASE_THREADS=$th OMP_NUM_THREADS=$th \
    OMP_PROC_BIND=true OMP_PLACES=cores /tmp/bench_a_base "$@" > "$f" 2>&1
  local ts rep
  ts=$(grep -m1 '^TSET' "$f")
  rep=$(grep '^REP' "$f" | tail -n +2 | awk -F, '{print $3}' | sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')
  printf '%-22s mask=%-12s threads=%s  median_ms=%-10s %s\n' \
    "$tag" "$mask" "$th" "$rep" "$ts"
}

echo "=== replication (5 reps; recorded medians in parentheses)"
echo "curve_w1_c0-5_t6     (162.8 ms)"
run curve_w1_c0-5_t6     0-5 6 mult w1 5
echo "curve_w1_call_t12    (138.8 ms)"
run curve_w1_call_t12    all 12 mult w1 5
echo "curve_w1_c0-3_t4     (193.4 ms)"
run curve_w1_c0-3_t4     0-3 4 mult w1 5
echo "curve_sib_w1_c0+2+4_t3 (269.8 ms @ c0,2 w/ 6thr; sib c0,2,4,t3 has no twin)"
run curve_sib_w1_c0+2+4_t3 0,2,4 3 mult w1 5
echo "curve_w3_c0-5_t6     (4550.4 ms)"
run curve_w3_c0-5_t6     0-5 6 mult w3 3
