#!/usr/bin/env bash
# _noisecheck.sh -- how much of the v2-vs-v1 curve difference is measurement
# noise?  Re-runs two points that the binding change should NOT affect (1 CPU
# cannot use more than 1 CPU either way) and one that it should (6 CPUs), on an
# otherwise idle machine, then prints medians.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1

med() { # med <file> -- median of REP col 3, rep 0 discarded
  grep '^REP' "$1" | tail -n +2 | awk -F, '{print $3}' | sort -n \
    | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'
}

one() { # one <label> <mask> <threads> <wl> <reps>
  local label=$1
  local mask=$2
  local th=$3
  local wl=$4
  local reps=$5
  local f="/tmp/nc_${label}.txt"
  taskset -c "$mask" env PHASE_THREADS=$th OMP_NUM_THREADS=$th \
    /tmp/bench_a_base mult "$wl" "$reps" > "$f" 2>&1
  printf '%-22s mask=%-6s threads=%-3s median=%-9s (%s)\n' \
    "$label" "$mask" "$th" "$(med "$f")" "$(grep -m1 '^TSET' "$f")"
}

{
  echo "idle-machine re-check (v2 recorded: w1 c0 561.7 | c0-5 131.8 | call 110.6)"
  one w1_c0_t1 0 1 w1 7
  one w2_c0_t1 0 1 w2 7
  one w1_c0-5_t6 0-5 6 w1 7
  one w1_call_t12 0-11 12 w1 7
} 2>&1
