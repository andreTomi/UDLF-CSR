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
#   5. RDPAC port: TU check + S1-S5 parity gate + corner cases.
#   6. RDPAC thread determinism: strict md5 equality across 1/2/4/8/12
#      threads (parity) and 1/12 (corner) -- mismatch aborts the script.
#   7. RDPAC serial-vs-parallel bench under OMP_WAIT_POLICY=PASSIVE
#      (libgomp's default ACTIVE spin-wait starves SMT siblings on this
#      6-core/12-thread box; see bench_rdpac.cpp header for numbers).
# NOTE: paths are hardcoded to the WSL mount of this repo; invoke as
# `bash Source/tests/run_all.sh` (shell variables inside the file are fine).
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
echo "=== RDPAC thread determinism (strict: all md5 must be identical) ==="
ref_md5=""
det_ok=1
for t in 1 2 4 8 12; do
  m=$(OMP_NUM_THREADS=$t /tmp/test_rdpac_parity | grep -v Build | md5sum | cut -d' ' -f1)
  echo "  parity t=$t md5=$m"
  if [ -z "$ref_md5" ]; then ref_md5=$m; elif [ "$m" != "$ref_md5" ]; then det_ok=0; fi
done
m1=$(OMP_NUM_THREADS=1 /tmp/test_rdpac_corner | grep -v Build | md5sum | cut -d' ' -f1)
m12=$(OMP_NUM_THREADS=12 /tmp/test_rdpac_corner | grep -v Build | md5sum | cut -d' ' -f1)
echo "  corner t=1  md5=$m1"
echo "  corner t=12 md5=$m12"
if [ "$m1" != "$m12" ]; then det_ok=0; fi
if [ "$det_ok" -ne 1 ]; then
  echo "RDPAC thread determinism FAILED: md5 mismatch across thread counts"
  exit 1
fi
echo "=== RDPAC thread determinism: OK ==="
echo "=== RDPAC serial-vs-parallel bench (checksums must match) ==="
echo "   OMP_WAIT_POLICY=PASSIVE: with libgomp's default ACTIVE spin-wait the"
echo "   12 waiting threads starve their 6 SMT siblings inside multiply_l1's"
echo "   per-level barriers (n=2000: 0.45x, parallel slower than serial);"
echo "   PASSIVE sleeps instead. Results stay bit-exact under both policies."
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/bench/bench_rdpac.cpp -o /tmp/bench_rdpac || exit 1
OMP_WAIT_POLICY=PASSIVE /tmp/bench_rdpac 2000 64 8 42 || exit 1
OMP_WAIT_POLICY=PASSIVE /tmp/bench_rdpac 10000 100 15 42 || exit 1
echo "=== RDPAC gates: OK ==="
