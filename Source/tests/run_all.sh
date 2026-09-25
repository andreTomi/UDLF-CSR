#!/usr/bin/env bash
# run_all.sh -- driver for the multiply_and_prune adaptive accumulator suite.
#
# Usage (WSL, from repo root):
#   bash Source/tests/run_all.sh
#
# Requires: g++ with OpenMP (libgomp) -- nothing else. S2.3 step 2
# removed the builder's std::execution/par_unseq sort and the TBB arena,
# so these binaries link no TBB.
#
# Stages:
#   1. Build unit suite + e2e bench + instantiation (compile-time) check.
#   2. Unit suite (dense/hash branches vs independent reference, edge cases).
#   3. A9 thread determinism: same suite at 1/2/4/8 threads, md5 must match.
#   4. E2E sweep: original dense kernel vs adaptive, per-process peak RSS.
# NOTE: paths are hardcoded to the WSL mount of this repo; the script is
# intentionally free of shell variables (quoting-safe under wsl.exe).
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1

echo "=== Building..."
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/test_map || exit 1
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_e2e.cpp -o /tmp/bench_e2e || exit 1
g++ -std=gnu++2b -fsyntax-only -fopenmp -I Source/src Source/tests/check_instantiations.cpp || exit 1
echo "=== Instantiation check: OK"

echo "=== Unit suite: adaptive multiply_and_prune ==="
/tmp/test_map | tail -n +2 | grep -v Build

echo "=== Characterization suite: build/merge/combine/transpose ==="
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_csr_ops.cpp -o /tmp/test_csr_ops || exit 1
/tmp/test_csr_ops | tail -n +2 | grep -v Build

echo "=== C-thread determinism: characterization suite at 1/2/4/8/12 threads ==="
echo "   (all five md5 must be identical; the header prints the thread count)"
for t in 1 2 4 8 12; do
  OMP_NUM_THREADS=$t /tmp/test_csr_ops | tail -n +2 | grep -v Build | md5sum
done

echo "=== A9 thread determinism (all four md5 must be identical) ==="
OMP_NUM_THREADS=1 /tmp/test_map | tail -n +2 | grep -v Build | md5sum
OMP_NUM_THREADS=2 /tmp/test_map | tail -n +2 | grep -v Build | md5sum
OMP_NUM_THREADS=4 /tmp/test_map | tail -n +2 | grep -v Build | md5sum
OMP_NUM_THREADS=8 /tmp/test_map | tail -n +2 | grep -v Build | md5sum

echo "=== E2E sweep: original dense kernel vs adaptive (per-process RSS) ==="
/tmp/bench_e2e dense 10000
/tmp/bench_e2e adaptive 10000
/tmp/bench_e2e dense 100000
/tmp/bench_e2e adaptive 100000
/tmp/bench_e2e dense 1000000
/tmp/bench_e2e adaptive 1000000
/tmp/bench_e2e dense 10000000
/tmp/bench_e2e adaptive 10000000
/tmp/bench_e2e dense 50000000
/tmp/bench_e2e adaptive 50000000

echo "=== RDPAC port: TU check (factory registration, header-only method) ==="
g++ -std=gnu++23 -fsyntax-only -fopenmp -I Source/src -I Source/src/Config/hpp -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp Source/tests/check_rdpac_tu.cpp || exit 1
echo "=== RDPAC port: S1-S5 phase parity gate (core vs frozen verbatim reference) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/tests/test_rdpac_parity.cpp -o /tmp/test_rdpac_parity || exit 1
/tmp/test_rdpac_parity || exit 1
echo "=== RDPAC port: corner cases (multi-seed, 2L==N, ties, pin, contracts, p=0) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/tests/test_rdpac_corner.cpp -o /tmp/test_rdpac_corner || exit 1
/tmp/test_rdpac_corner || exit 1
echo "=== RDPAC gates: OK ==="
