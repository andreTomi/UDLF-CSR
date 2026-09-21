#!/usr/bin/env bash
# _bindcheck.sh -- does the Etapa A harness's OMP_PROC_BIND=true + OMP_PLACES=cores
# shrink TBB's arena (and therefore the build sort, which runs on TBB and ignores
# OMP_NUM_THREADS) below the OMP thread count?
#
# Same mask (no taskset => all 12 CPUs), same PHASE_THREADS/OMP_NUM_THREADS=12;
# the only difference is the binding. The TSET line reports the arena width that
# the driver actually saw (tbb_max = global_control, max_concurrency = arena).
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
OUT=Data/phase_profile/etapa_a/bindcheck.txt

g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src \
    Source/bench/bench_phase.cpp -o /tmp/bc_pp -ltbb || exit 1

median() { # median() -- REP line col 3, rep 0 discarded
  grep '^REP' "$1" | tail -n +2 | awk -F, '{print $3}' | sort -n \
    | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'
}

{
  echo "### 12 CPUs (no taskset), 12 threads; only the binding differs"
  for wl in w1 w3; do
    for mode in nobind bind; do
      f=/tmp/bc_${wl}_${mode}.txt
      if [ "$mode" = bind ]; then
        env PHASE_THREADS=12 OMP_NUM_THREADS=12 OMP_PROC_BIND=true OMP_PLACES=cores \
            /tmp/bc_pp mult "$wl" 3 > "$f" 2>&1
      else
        env PHASE_THREADS=12 OMP_NUM_THREADS=12 \
            /tmp/bc_pp mult "$wl" 3 > "$f" 2>&1
      fi
      printf '%-4s %-7s median=%-10s (%s)\n' "$wl" "$mode" "$(median "$f")" \
             "$(grep -m1 '^TSET' "$f")"
      grep -E 'B2_SORT|BUILD_CALL|MP_CALL|F1_ACCUM|F2_PRUNE|MULT_TOTAL' "$f" \
        | sed 's/^/       /'
      echo
    done
  done
  echo "### same, but the ledger total (pipe) with and without the binding"
  for mode in nobind bind; do
    f=/tmp/bc_pipe_${mode}.txt
    if [ "$mode" = bind ]; then
      env PHASE_THREADS=12 OMP_NUM_THREADS=12 OMP_PROC_BIND=true OMP_PLACES=cores \
          /tmp/bc_pp pipe w1 3 > "$f" 2>&1
    else
      env PHASE_THREADS=12 OMP_NUM_THREADS=12 /tmp/bc_pp pipe w1 3 > "$f" 2>&1
    fi
    printf 'pipe w1 %-7s median=%-10s\n' "$mode" "$(median "$f")"
  done
} > "$OUT" 2>&1
echo "written $OUT"
