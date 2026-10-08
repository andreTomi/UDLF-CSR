#!/usr/bin/env bash
# step1_thread_bisect.sh -- is the H5 crash thread-count dependent?
#
# Symptom: test_rfe_hyp dies with SIGSEGV inside map_to_csr's
# build_from_partial_results on the SIXTH matrix built in the process, after the
# "[BuildV2] Total triplets: 5461" line prints. The standalone probe
# (step1_debug_map_to_csr.sh) builds the same matrix once and survives, so the
# input and the function are fine in isolation.
#
# Two candidate causes, and this bisects between them:
#   (a) THREAD-DEPENDENT bug in the engine's build under >1 OMP threads -- the
#       probe only ever ran single-threaded;
#   (b) cumulative heap state from five prior builds.
# Running the real binary at 1/2/4/12 threads separates them: if 1 thread
# survives and 2+ die, it is (a) and belongs in the engine investigation; if all
# die it is (b) and is local to the test.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/step1_thbisect}
FLAGS=${FLAGS:-"-std=gnu++2b -O0 -g -ffp-contract=off -fopenmp"}

$CXX $FLAGS -I Source/src -I Source/src/Core/hpp -I Source/tests \
  Source/tests/test_rfe_hyp.cpp -o "$BIN" || exit 1

for th in 1 2 4 12; do
  OMP_NUM_THREADS=$th "$BIN" > "/tmp/step1_th$th.txt" 2>&1
  rc=$?
  last=$(grep -v BuildV2 "/tmp/step1_th$th.txt" | tail -1 | cut -c1-70)
  printf 'threads=%-3s exit=%-4s last: %s\n' "$th" "$rc" "$last"
done

echo
echo "-- how far did each run get (last H-line emitted)? --"
for th in 1 2 4 12; do
  printf 'threads=%-3s last-H: %s\n' "$th" \
    "$(grep -o '^H[0-9b]* ' "/tmp/step1_th$th.txt" | tail -1)"
done