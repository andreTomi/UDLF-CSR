#!/usr/bin/env bash
# run_etapa_a.sh -- Etapa A (A1/A2/A3/A5) campaign driver.
#
# Runs the synthetic workloads under taskset with OpenMP+TBB pinned to the same
# thread count (bench_phase honours PHASE_THREADS), captures raw stdout per
# configuration into <outdir>/raw/, and records the CPU topology + commit.
#
# Usage (WSL, from repo root):
#   python3 Source/bench/fix_crlf.py Source/bench/run_etapa_a.sh   # -> /tmp
#   bash /tmp/run_etapa_a.sh <outdir> build          # compile bins
#   bash /tmp/run_etapa_a.sh <outdir> <stage> [...]  # stages:
#     curve_fast curve_w3a curve_w3b phases_fast phases_w3a phases_w3b
#     sib pipe_w1 pipe_w3 mem real
# Stages are separate so each WSL invocation stays under the ~5 min command
# budget (w3 at 1 core is ~19 s/rep and dominates the runtime).
#
# AFFINITY GUARD (why this file is shaped like this):
#   `taskset -c <mask>` can only *restrict* the caller's CPU set, never expand
#   it. If the invoking shell (wsl.exe, inheriting from the Windows console) is
#   itself limited to 2 CPUs, `taskset -c 0-5 cmd` runs cmd on 2 CPUs and the
#   curve silently measures the wrong configuration. A batch of this campaign
#   was corrupted exactly that way: 32 captures labelled c0-3/c0-5/call all
#   reported affinity=0,1. Every run now goes through check_mask(), which
#   compares the child's usable CPU count with the requested one and ABORTS the
#   stage on mismatch (MASKFAIL goes to <outdir>/mask_audit.log).
#   In every spec below the thread count equals the number of CPUs in the mask,
#   so `taskset -c $cpus nproc` must equal $th.
#
# Notes:
#   * cpusets follow the request (0 / 0-1 / 0-3 / 0-5 / all) PLUS the
#     sibling-aware sets on this i5-10400F (cpu pairs 0-1, 2-3, ... share a
#     physical core), so the curve has both readings.
#   * two instrumented builds: -DPHASE_PROFILE (sample=1) and
#     -DPHASE_PROFILE -DPHASE_PROFILE_ROW_SAMPLE=8 (sample=8, ~0 overhead).
#     Totals come from the no-flag build; phase tables from the flag builds.
#   * EA_REPS overrides the repetition count, so a smoke run is
#     `EA_REPS=2 bash /tmp/run_etapa_a.sh /tmp/smoke curve_fast`.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
set -u
REPO=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
OUT=${1:-/tmp/etapa_a}
shift 2>/dev/null || true
STAGES=${@:-"build curve_fast curve_w3a curve_w3b phases_fast phases_w3a phases_w3b sib pipe_w1 pipe_w3 mem"}
mkdir -p "$OUT/raw"
cd "$REPO" || exit 1

R_FAST=${EA_REPS:-7}
R_W3=${EA_REPS:-7}
R_PIPE=${EA_REPS:-5}
R_REAL=${EA_REPS:-5}

stage_wanted() { case " $STAGES " in *" $1 "*) return 0;; *) return 1;; esac; }

if stage_wanted build; then
  echo "=== Building drivers"
  g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp -o /tmp/bench_a_base -ltbb || exit 1
  g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/bench/bench_phase.cpp -o /tmp/bench_a_s1 -ltbb || exit 1
  g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -DPHASE_PROFILE_ROW_SAMPLE=8 -I Source/src Source/bench/bench_phase.cpp -o /tmp/bench_a_s8 -ltbb || exit 1
fi

{
  echo "# lscpu -e (cpu,core,socket,node)"
  lscpu -e=cpu,core,socket,node
  echo "# lscpu summary"
  lscpu | grep -iE 'model name|thread\(s\) per core|core\(s\) per socket|socket\(s\)|L3|NUMA node\(s\)'
  echo "# online/possible"
  echo "online=$(cat /sys/devices/system/cpu/online) possible=$(cat /sys/devices/system/cpu/possible)"
  echo "# nproc --all / nproc"
  echo "nproc_all=$(nproc --all) nproc=$(nproc)"
  echo "# mem"
  head -3 /proc/meminfo
  echo "# kernel"
  uname -r
  echo "# git"
  git --no-pager log --oneline -1
} > "$OUT/machine.txt" 2>&1

MASK_LOG="$OUT/mask_audit.log"
: > "$MASK_LOG"

check_mask() { # check_mask <cpuset|all> <expected_count> -> 0 ok, 1 bad
  local cpus=$1 want=$2 got
  if [ "$cpus" = "all" ]; then
    got=$(nproc)
  else
    got=$(taskset -c "$cpus" nproc 2>/dev/null) || got=0
  fi
  if [ "$got" != "$want" ]; then
    printf 'MASKFAIL,mask=%s,want=%s,got=%s,pid=%d\n' "$cpus" "$want" "$got" "$$" \
      | tee -a "$MASK_LOG" >&2
    return 1
  fi
  printf 'MASKOK,mask=%s,count=%s\n' "$cpus" "$got" >> "$MASK_LOG"
  return 0
}

run() { # run <bin> <tag> <taskset-cpus> <threads> <args...>
  local bin=$1 tag=$2 cpus=$3 th=$4; shift 4
  local f="$OUT/raw/$tag.txt"
  # BINDING (A1 finding): OMP_PROC_BIND=true + OMP_PLACES=cores does NOT do what
  # it looks like here. On this WSL2 box it re-pins the master thread to its
  # place (core 0 = CPUs 0,1) and TBB -- which the build sort runs on, and which
  # sizes its arena from the affinity at first use -- collapses to
  # max_concurrency=2. Measured with the same mask and the same thread count
  # (bench_phase mult w1 3, 12 threads, all CPUs): 117.4 ms unbound vs 213.9 ms
  # bound, with B2_SORT 24.8 ms -> 68.9 ms. Default is therefore NO binding;
  # set EA_BIND=1 to reproduce the old (crippled) numbers.
  local bind_env=""
  [ "${EA_BIND:-0}" = "1" ] && bind_env="OMP_PROC_BIND=true OMP_PLACES=cores"
  check_mask "$cpus" "$th" || { echo "ABORT: $tag"; return 1; }
  echo "[$(date +%H:%M:%S)] $tag  ($cpus, T=$th, bind=${EA_BIND:-0}, $*)"
  if [ "$cpus" = "all" ]; then
    env $bind_env PHASE_MASK="all" PHASE_THREADS=$th OMP_NUM_THREADS=$th \
      "$bin" "$@" > "$f" 2>&1
  else
    taskset -c "$cpus" env $bind_env PHASE_MASK="$cpus" PHASE_THREADS=$th \
      OMP_NUM_THREADS=$th "$bin" "$@" > "$f" 2>&1
  fi
}


# ---- A1: totals curve (no flag) -------------------------------------------
for wl in w1 w2; do
  stage_wanted curve_fast || continue
  for spec in "0:1" "0-1:2" "0-3:4" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_base "curve_${wl}_c${cpus}_t${th}" "$cpus" "$th" mult "$wl" "$R_FAST" \
      || exit 90
  done
done
if stage_wanted curve_w3a; then
  for spec in "0:1" "0-1:2"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_base "curve_w3_c${cpus}_t${th}" "$cpus" "$th" mult w3 "$R_W3" \
      || exit 90
  done
fi
if stage_wanted curve_w3b; then
  for spec in "0-3:4" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_base "curve_w3_c${cpus}_t${th}" "$cpus" "$th" mult w3 "$R_W3" \
      || exit 90
  done
fi

# ---- A1: phase tables (flag) ----------------------------------------------
for wl in w1 w2; do
  stage_wanted phases_fast || continue
  for spec in "0:1" "0-1:2" "0-3:4" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    bin=/tmp/bench_a_s1
    if [ "$th" -ge 4 ]; then bin=/tmp/bench_a_s8; fi
    run "$bin" "phases_${wl}_c${cpus}_t${th}" "$cpus" "$th" mult "$wl" "$R_FAST" \
      || exit 90
  done
done
if stage_wanted phases_w3a; then
  for spec in "0:1" "0-1:2"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_s1 "phases_w3_c${cpus}_t${th}" "$cpus" "$th" mult w3 "$R_W3" \
      || exit 90
  done
fi
if stage_wanted phases_w3b; then
  for spec in "0-3:4" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_s1 "phases_w3_c${cpus}_t${th}" "$cpus" "$th" mult w3 "$R_W3" \
      || exit 90
  done
fi

# ---- A1: sibling-aware (1 physical core per step) -------------------------
if stage_wanted sib; then
  for spec in "0,2:2" "0,2,4:3" "0,2,4,6:4" "0,2,4,6,8,10:6"; do
    cpus=${spec%%:*}; th=${spec##*:}
    tag=${cpus//,/+}
    run /tmp/bench_a_base "curve_sib_w1_c${tag}_t${th}" "$cpus" "$th" mult w1 "$R_FAST" \
      || exit 90
    run /tmp/bench_a_base "curve_sib_w2_c${tag}_t${th}" "$cpus" "$th" mult w2 "$R_FAST" \
      || exit 90
  done
  for spec in "0,2:2" "0,2,4:3" "0,2,4,6:4" "0,2,4,6,8,10:6"; do
    cpus=${spec%%:*}; th=${spec##*:}
    tag=${cpus//,/+}
    run /tmp/bench_a_base "curve_sib_w3_c${tag}_t${th}" "$cpus" "$th" mult w3 "$R_PIPE" \
      || exit 90
  done
  run /tmp/bench_a_base "curve_sib_w3_call_t12" "all" 12 mult w3 "$R_PIPE" || exit 90
fi


# ---- A2: pipe ledger ------------------------------------------------------
if stage_wanted pipe_w1; then
  for spec in "0:1" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    bin=/tmp/bench_a_s8
    if [ "$th" = "1" ]; then bin=/tmp/bench_a_s1; fi
    run "$bin" "pipe_w1_c${cpus}_t${th}" "$cpus" "$th" pipe w1 "$R_PIPE" || exit 90
  done
fi
if stage_wanted pipe_w3; then
  for spec in "0:1" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_s1 "pipe_w3_c${cpus}_t${th}" "$cpus" "$th" pipe w3 "$R_PIPE" \
      || exit 90
  done
fi

# ---- A3: memory decomposition (PHASE_MEM=1) -------------------------------
if stage_wanted mem; then
  for wl in w1 w3; do
    for spec in "0:1" "all:12"; do
      cpus=${spec%%:*}; th=${spec##*:}
      tag="mem_${wl}_c${cpus}_t${th}"
      check_mask "$cpus" "$th" || exit 90
      f="$OUT/raw/$tag.txt"
      echo "[$(date +%H:%M:%S)] $tag"
      if [ "$cpus" = "all" ]; then
        PHASE_MEM=1 PHASE_THREADS=$th OMP_NUM_THREADS=$th /tmp/bench_a_s1 mult "$wl" 3 > "$f" 2>&1
      else
        taskset -c "$cpus" env PHASE_MEM=1 PHASE_THREADS=$th OMP_NUM_THREADS=$th \
          /tmp/bench_a_s1 mult "$wl" 3 > "$f" 2>&1
      fi
    done
  done
fi

# ---- A5: real dataset (ALOI, LHRR) ----------------------------------------
if stage_wanted real; then
  BIN=${EA_BIN:-/tmp/udlf_rk_aloi_N72000_L400.bin}
  N=${EA_N:-72000}; L=${EA_L:-400}; K=${EA_K:-10}; LIM=${EA_LIM:-400}
  for spec in "0:1" "0-1:2" "0-3:4" "0-5:6" "all:12"; do
    cpus=${spec%%:*}; th=${spec##*:}
    run /tmp/bench_a_s1 "real_lhrr_aloi_c${cpus}_t${th}" "$cpus" "$th" \
      real "$BIN" "$N" "$L" "$K" "$LIM" lhrr "$R_REAL" || exit 90
  done
fi

echo "=== Done. Raw output in $OUT/raw"
ls "$OUT/raw" | wc -l
