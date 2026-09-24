#!/bin/bash
# S2.1 equivalence matrix: builder (v2 / legacy) x row-sort (idx / zip) x threads,
# on the synthetic workloads. Prints the md5 of the canonical dumps so the gate
# is decided by bytes, not by the driver's weak checksum.
#
# Usage: bash Source/bench/s2_dump_matrix.sh [outdir] [workload]
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
OUT=${1:-/home/andre/dmp_matrix}
WL=${2:-w1}
BIN=/home/andre/udlf_bin/bp_s2
mkdir -p "$OUT"
cd "$R"

echo "== workload $WL: builder x rowsort x threads -> md5(ab.bin, result.bin)"
for b in v2 legacy; do
  for s in idx zip; do
    for t in 1 4 12; do
      d="$OUT/${WL}_${b}_${s}_t${t}"
      mkdir -p "$d"
      taskset -c 0-11 env OMP_NUM_THREADS=$t PHASE_THREADS=$t \
# S2.3 p2: PHASE_BUILDER is ignored by the post-removal driver (the
# legacy arm is gone); this harness needs commit <= 1f56845.
        PHASE_BUILDER=$b PHASE_ROWSORT=$s PHASE_DUMP_DIR="$d" \
        "$BIN" mult "$WL" 1 > "$d/log.txt" 2>&1
      printf '%-38s ' "${WL}_${b}_${s}_t${t}"
      md5sum "$d/ab.bin" | cut -d' ' -f1 | tr '\n' ' '
      md5sum "$d/result.bin" | cut -d' ' -f1 | tr '\n' ' '
      grep -h '^REP,' "$d/log.txt" | cut -d, -f4 | tr '\n' ' '
      echo
    done
  done
done
echo "(columns: ab.bin md5 | result.bin md5 | nnz)"
