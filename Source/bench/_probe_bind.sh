#!/usr/bin/env bash
# _probe_bind.sh -- is the TSET affinity field corrupted by OMP_PROC_BIND?
# TSET is printed AFTER the matrices are built (a parallel region runs first),
# so if OMP_PROC_BIND=true + OMP_PLACES=cores re-pins the master thread, the
# field reports the master's post-bind affinity, not the taskset mask.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
probe() { # probe <label> <env...> -- args after label are env assignments
  local label=$1; shift
  local ts
  ts=$(env "$@" taskset -c 0-5 "$BIN" mult w1 1 2>/dev/null | grep -m1 '^TSET')
  printf '%-42s %s\n' "$label" "$ts"
}
BIN=/tmp/bench_a_base
echo "== mask 0-5, 6 threads =="
probe "no PROC_BIND"                 OMP_NUM_THREADS=6 PHASE_THREADS=6
probe "PROC_BIND=true PLACES=cores"  OMP_NUM_THREADS=6 PHASE_THREADS=6 OMP_PROC_BIND=true OMP_PLACES=cores
probe "PROC_BIND=false"              OMP_NUM_THREADS=6 PHASE_THREADS=6 OMP_PROC_BIND=false
echo
echo "== mask 0,2,4 (3 cores, 3 threads) =="
probe2() {
  local label=$1; shift
  local ts
  ts=$(env "$@" taskset -c 0,2,4 "$BIN" mult w1 1 2>/dev/null | grep -m1 '^TSET')
  printf '%-42s %s\n' "$label" "$ts"
}
probe2 "no PROC_BIND"                 OMP_NUM_THREADS=3 PHASE_THREADS=3
probe2 "PROC_BIND=true PLACES=cores"  OMP_NUM_THREADS=3 PHASE_THREADS=3 OMP_PROC_BIND=true OMP_PLACES=cores
echo
echo "== caller restricted to 0,1 and mask 0-3 =="
taskset -c 0,1 bash -c 'env OMP_NUM_THREADS=4 PHASE_THREADS=4 taskset -c 0-3 /tmp/bench_a_base mult w1 1 2>/dev/null | grep -m1 "^TSET"'
