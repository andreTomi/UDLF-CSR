#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CXX=${CXX:-g++}
BIN=${CPRR_BENCH_BIN:-/tmp/cprr_sparse_serial_benchmark}
OPENMP=${CPRR_OPENMP:-0}

OPENMP_FLAGS=()
if [[ "$OPENMP" == 1 ]]; then
  OPENMP_FLAGS=(-fopenmp)
fi

"$CXX" -std=gnu++23 -O3 -g -fno-omit-frame-pointer -ffp-contract=off "${OPENMP_FLAGS[@]}" \
  -I Source/src/Methods/hpp ports/CPRR/benchmark_serial.cpp -o "$BIN"
exec "$BIN" "$@"