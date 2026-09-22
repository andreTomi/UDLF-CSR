#!/bin/bash
# S1-bis: same-session interleaved A/B for the Fase 1 verdict, with the
# permanent protocol policy:
#   (1) discard the FIRST block of the session  (cold .bin / cold pages)
#   (2) discard rep 0 of every block            (first-touch of a fresh process)
# plus one throwaway warm-up run per binary before any measurement, a canary
# run (fixed load, single binary) before/after to size session drift, and
# REPRU lines (per-rep page-fault deltas) as *mechanism* evidence for the
# rep-0 penalty -- not just the observed pattern.
#
# Both headers come from git (base = pre-Fase-1 kernel, head = the committed
# Fase-1 kernel), so this harness is immune to uncommitted working-tree edits.
# Output goes to the repo (persistent): WSL2 drops /tmp when the distro idles
# out, and every step is skipped if its output file already exists, so a killed
# run resumes instead of restarting.
#
# Usage: bash Source/bench/s1_bis_run.sh [outdir]
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
OUT=${1:-$R/Data/phase_profile/etapa_b/ordem/s1_bis}
BASE_COMMIT=${BASE_COMMIT:-86898d5}       # pre-Fase-1 kernel
HEAD_COMMIT=${HEAD_COMMIT:-3c08bd1}       # committed Fase-1 kernel (with the fix)
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
N=72000; L=400; K=10; LIMIT=400; REPS=2
mkdir -p "$OUT/raw"
cd "$R"

for v in base head; do
  mkdir -p /tmp/inc_s1bis_$v/Core/hpp
  cp Source/src/Core/hpp/*.hpp /tmp/inc_s1bis_$v/Core/hpp/
  c=$BASE_COMMIT; [ "$v" = head ] && c=$HEAD_COMMIT
  git show "$c:Source/src/Core/hpp/SparseMatrix.hpp" \
      > /tmp/inc_s1bis_$v/Core/hpp/SparseMatrix.hpp
  g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -DPHASE_PROFILE \
      -I /tmp/inc_s1bis_$v -I Source/src \
      Source/bench/bench_phase.cpp -o /tmp/s1bis_$v -ltbb
  echo "BUILT $v ($(git rev-parse --short $c)) -> /tmp/s1bis_$v"
done

have() { [ -s "$OUT/raw/$1.txt" ]; }

run() {  # run <binary> <tag>
  have "$2" && { echo "skip $2"; return; }
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
    "$1" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/raw/$2.txt" 2>&1
  echo "ran $2"
}
canary() {  # canary <binary> <tag>
  have "$2" && { echo "skip $2"; return; }
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
    "$1" mult w1 2 > "$OUT/raw/$2.txt" 2>&1
  echo "ran $2"
}

echo "--- warm-up (discarded, one per binary)"
run /tmp/s1bis_base warm_base
run /tmp/s1bis_head warm_head
canary /tmp/s1bis_head canary_0

echo "--- pair 1: base, head"
run /tmp/s1bis_base b1_base
run /tmp/s1bis_head b2_head
echo "--- pair 2: head, base"
run /tmp/s1bis_head b3_head
run /tmp/s1bis_base b4_base
echo "--- pair 3: base, head"
run /tmp/s1bis_base b5_base
run /tmp/s1bis_head b6_head

canary /tmp/s1bis_head canary_1
echo "DONE -> $OUT"
