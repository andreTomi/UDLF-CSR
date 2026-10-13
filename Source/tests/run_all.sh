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
#   8. BFSTree framework gates (registered method, strict regime): TU check,
#      parity vs the legacy's recorded pins, corner cases, strict md5 equality
#      across 1/2/4/8/12 threads, serial-vs-parallel bench (PASSIVE) and
#      ASan+UBSan. Additive: stages 1-7 and their historical md5s untouched.
# NOTE: the script cds to its own repo root (two levels up from this file), so
# it runs from any mount of the repo; no absolute path is baked in.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd "$(dirname "$0")/../.." || exit 1

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

# BFSTree port (path CSR A2/A4). Tres gates em ~13 s: a hipotese F5=SpGEMM
# (45 checks, T1/T3 bit-exatos celula a celula), o contrato de entrada do
# carregador de listas reais (recusa rc=5 linha com < L sobreviventes e rc=4
# linha sem self -- foi o rc=5 que impediu oito runs em N=13k/18k reais de
# rodarem sobre lixo) e a bit-exactness de A/A2/A4 contra o golden que o proprio
# legado grava, com o CK_FINAL pinado. O gate resolve o root do repo sozinho.
echo "=== BFSTree port: hypothesis + input contract + bit-exactness vs golden ==="
bash Source/tests/gate_bfstree.sh || exit 1
echo "=== BFSTree gates: OK ==="

# ---- Stage 8 (ADITIVA): BFSTree no framework (registro em Source/src) -------
# Metade framework do padrao RDPAC (etapas 5/6/7): TU check, paridade contra os
# pins gravados do legado, casos-limite, determinismo estrito de threads
# (1/2/4/8/12), serial-vs-parallel sob PASSIVE e ASan/UBSan.
# NAO toca as etapas 1-7 nem os md5 historicos: os 99 checks (b6a4f54e /
# 112fb487) sao produzidos pelas suites das etapas 1-2, com linha de compilacao
# propria, e continuam rodando ali.
echo "=== BFSTree framework: TU check (factory registration, header-only) ==="
g++ -std=gnu++23 -fsyntax-only -fopenmp -I Source/src -I Source/src/Config/hpp -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp Source/tests/check_bfstree_tu.cpp || exit 1

echo "=== BFSTree framework: build guard (strict ALLOWED / fast-math REFUSED) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Config/hpp -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp Source/tests/check_bfstree_guard.cpp -o /tmp/check_bfstree_guard_strict || exit 1
g++ -std=gnu++2b -O2 -fopenmp -ffast-math -I Source/src -I Source/src/Config/hpp -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp Source/tests/check_bfstree_guard.cpp -o /tmp/check_bfstree_guard_fastmath || exit 1
/tmp/check_bfstree_guard_strict   | grep -q "GUARD ALLOWED" || { echo "BFSTree guard FAILED: strict build must ALLOW"; exit 1; }
/tmp/check_bfstree_guard_fastmath | grep -q "GUARD REFUSED" || { echo "BFSTree guard FAILED: fast-math build must REFUSE"; exit 1; }
echo "  strict -> GUARD ALLOWED; fast-math -> GUARD REFUSED"

echo "=== BFSTree framework: parity vs legacy pins + corner cases ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/tests/test_bfstree_parity.cpp -o /tmp/test_bfstree_parity || exit 1
/tmp/test_bfstree_parity > /tmp/bf_par.out 2>&1 || { tail -40 /tmp/bf_par.out; exit 1; }
grep -v BuildV2 /tmp/bf_par.out
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/tests/test_bfstree_corner.cpp -o /tmp/test_bfstree_corner || exit 1
/tmp/test_bfstree_corner > /tmp/bf_cor.out 2>&1 || { tail -40 /tmp/bf_cor.out; exit 1; }
grep -v BuildV2 /tmp/bf_cor.out

echo "=== BFSTree thread determinism (strict: all md5 must be identical) ==="
bfstree_ref_md5=""
bfstree_det_ok=1
for t in 1 2 4 8 12; do
  m=$(OMP_NUM_THREADS=$t /tmp/test_bfstree_parity | grep -v BuildV2 | md5sum | cut -d' ' -f1)
  echo "  parity t=$t md5=$m"
  if [ -z "$bfstree_ref_md5" ]; then bfstree_ref_md5=$m; elif [ "$m" != "$bfstree_ref_md5" ]; then bfstree_det_ok=0; fi
done
[ "$bfstree_det_ok" = 1 ] || { echo "BFSTree thread determinism FAILED: md5 mismatch across thread counts"; exit 1; }
echo "=== BFSTree thread determinism: OK ==="

echo "=== BFSTree serial-vs-parallel bench (OMP_WAIT_POLICY=PASSIVE) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp Source/bench/bench_bfstree.cpp -o /tmp/bench_bfstree || exit 1
OMP_NUM_THREADS=1  OMP_WAIT_POLICY=PASSIVE /tmp/bench_bfstree A4 4000 640 10 42 256 4000 > /tmp/bf_t1.out  2>&1 || exit 1
OMP_NUM_THREADS=12 OMP_WAIT_POLICY=PASSIVE /tmp/bench_bfstree A4 4000 640 10 42 256 4000 > /tmp/bf_t12.out 2>&1 || exit 1
grep -E 'CK_FINAL=|BITFINAL=' /tmp/bf_t1.out  > /tmp/bf_t1.ck
grep -E 'CK_FINAL=|BITFINAL=' /tmp/bf_t12.out > /tmp/bf_t12.ck
cmp /tmp/bf_t1.ck /tmp/bf_t12.ck || { echo "BFSTree serial-vs-parallel checksum mismatch"; cat /tmp/bf_t1.ck /tmp/bf_t12.ck; exit 1; }
cat /tmp/bf_t1.ck

echo "=== BFSTree ASan/UBSan (parity + corner, 4 threads) ==="
g++ -std=gnu++2b -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer -I Source/src -I Source/src/Core/hpp Source/tests/test_bfstree_parity.cpp -o /tmp/test_bfstree_parity_san || exit 1
g++ -std=gnu++2b -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer -I Source/src -I Source/src/Core/hpp Source/tests/test_bfstree_corner.cpp -o /tmp/test_bfstree_corner_san || exit 1
OMP_NUM_THREADS=4 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 /tmp/test_bfstree_parity_san > /tmp/bf_san1.out 2>&1 || { tail -30 /tmp/bf_san1.out; exit 1; }
OMP_NUM_THREADS=4 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 /tmp/test_bfstree_corner_san > /tmp/bf_san2.out 2>&1 || { tail -30 /tmp/bf_san2.out; exit 1; }
if grep -q -e "ERROR: AddressSanitizer" -e "runtime error:" /tmp/bf_san1.out /tmp/bf_san2.out; then
  grep -a -m10 -e "ERROR: AddressSanitizer" -e "runtime error:" /tmp/bf_san1.out /tmp/bf_san2.out
  exit 1
fi
echo "=== BFSTree ASan/UBSan: OK ==="
echo "=== BFSTree framework gates: OK ==="

# Etapa 9: RFE framework gates (registered method, strict regime)
# TU check, parity vs legacy pins, corner cases, strict md5 equality
# across 1/2/4/8/12 threads, serial-vs-parallel bench (PASSIVE) and
# ASan+UBSan. Additive: stages 1-8 and their historical md5s untouched.

echo "=== RFE framework: TU check (factory registration, header-only method) ==="
g++ -std=gnu++23 -fsyntax-only -fopenmp -I Source/src -I Source/src/Config/hpp \
    -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp \
    -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp \
    Source/tests/check_rfe_tu.cpp || exit 1
echo "=== Instantiation check: OK ==="

echo "=== RFE strict-build guard (strict=ALLOW, fast-math=REFUSE) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Config/hpp \
    -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp \
    -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp \
    Source/tests/check_rfe_guard.cpp -o /tmp/check_rfe_guard_strict || exit 1
g++ -std=gnu++2b -O2 -fopenmp -ffast-math -I Source/src -I Source/src/Config/hpp \
    -I Source/src/Config/models -I Source/src/Core/hpp -I Source/src/Methods/hpp \
    -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp \
    Source/tests/check_rfe_guard.cpp -o /tmp/check_rfe_guard_fastmath || exit 1
/tmp/check_rfe_guard_strict   | grep -q "GUARD ALLOWED" || { echo "RFE guard FAILED: strict build must ALLOW"; exit 1; }
/tmp/check_rfe_guard_fastmath | grep -q "GUARD REFUSED" || { echo "RFE guard FAILED: fast-math build must REFUSE"; exit 1; }
echo "  strict -> GUARD ALLOWED; fast-math -> GUARD REFUSED"

echo "=== RFE framework: parity vs legacy pins + corner cases ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    Source/tests/test_rfe_parity.cpp -o /tmp/test_rfe_parity || exit 1
/tmp/test_rfe_parity > /tmp/rfe_par.out 2>&1 || { tail -40 /tmp/rfe_par.out; exit 1; }
grep -v BuildV2 /tmp/rfe_par.out

g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    Source/tests/test_rfe_corner.cpp -o /tmp/test_rfe_corner || exit 1
/tmp/test_rfe_corner > /tmp/rfe_cor.out 2>&1 || { tail -40 /tmp/rfe_cor.out; exit 1; }
grep -v BuildV2 /tmp/rfe_cor.out

echo "=== RFE thread determinism (strict: all md5 must be identical) ==="
rfe_ref_md5=""
rfe_det_ok=1
for t in 1 2 4 8 12; do
  m=$(OMP_NUM_THREADS=$t /tmp/test_rfe_parity | grep -v BuildV2 | md5sum | cut -d' ' -f1)
  echo "  parity t=$t md5=$m"
  if [ -z "$rfe_ref_md5" ]; then rfe_ref_md5=$m; elif [ "$m" != "$rfe_ref_md5" ]; then rfe_det_ok=0; fi
done
[ "$rfe_det_ok" = 1 ] || { echo "RFE thread determinism FAILED: md5 mismatch across thread counts"; exit 1; }
echo "=== RFE thread determinism: OK ==="

echo "=== RFE serial-vs-parallel bench (OMP_WAIT_POLICY=PASSIVE) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    Source/bench/bench_rfe_census.cpp -o /tmp/bench_rfe_census || exit 1
# ALOI rank-list cache: override with ALOI_BIN=... ; default tries the historical
# cache, then a committed/cached copy under the repo. Refuse loudly if absent
# rather than aborting on a bare path that only existed on the author's box.
ALOI_BIN=${ALOI_BIN:-/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin}
[ -f "$ALOI_BIN" ] || ALOI_BIN=Datasets/aloi/udlf_rk_aloi_N72000_L400.bin
[ -f "$ALOI_BIN" ] || { echo "RFE serial-vs-parallel: ALOI cache not found (set ALOI_BIN=...)"; exit 1; }
OMP_NUM_THREADS=1  OMP_WAIT_POLICY=PASSIVE /tmp/bench_rfe_census "$ALOI_BIN" 400 20 2 0.1 > /tmp/rfe_t1.out  2>&1 || exit 1
OMP_NUM_THREADS=12 OMP_WAIT_POLICY=PASSIVE /tmp/bench_rfe_census "$ALOI_BIN" 400 20 2 0.1 > /tmp/rfe_t12.out 2>&1 || exit 1
grep -E 'ck_rk=' /tmp/rfe_t1.out  > /tmp/rfe_t1.ck
grep -E 'ck_rk=' /tmp/rfe_t12.out > /tmp/rfe_t12.ck
cmp /tmp/rfe_t1.ck /tmp/rfe_t12.ck || { echo "RFE serial-vs-parallel checksum mismatch"; cat /tmp/rfe_t1.ck /tmp/rfe_t12.ck; exit 1; }
cat /tmp/rfe_t1.ck

echo "=== RFE ASan/UBSan (parity + corner, 4 threads) ==="
g++ -std=gnu++2b -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I Source/src -I Source/src/Core/hpp Source/tests/test_rfe_parity.cpp -o /tmp/test_rfe_parity_san || exit 1
g++ -std=gnu++2b -O1 -g -fopenmp -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I Source/src -I Source/src/Core/hpp Source/tests/test_rfe_corner.cpp -o /tmp/test_rfe_corner_san || exit 1
OMP_NUM_THREADS=4 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 /tmp/test_rfe_parity_san > /tmp/rfe_san1.out 2>&1 || { tail -30 /tmp/rfe_san1.out; exit 1; }
OMP_NUM_THREADS=4 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 /tmp/test_rfe_corner_san > /tmp/rfe_san2.out 2>&1 || { tail -30 /tmp/rfe_san2.out; exit 1; }
if grep -q -e "ERROR: AddressSanitizer" -e "runtime error:" /tmp/rfe_san1.out /tmp/rfe_san2.out; then
  grep -a -m10 -e "ERROR: AddressSanitizer" -e "runtime error:" /tmp/rfe_san1.out /tmp/rfe_san2.out
  exit 1
fi
echo "=== RFE ASan/UBSan: OK ==="
echo "=== RFE framework gates: OK ==="

echo "=== RFE CC golden (reference pins for the PERFORM_CCS port) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    -I Source/tests Source/tests/test_rfe_cc_golden.cpp -o /tmp/test_rfe_cc_golden || exit 1
/tmp/test_rfe_cc_golden > /tmp/rfe_cc_golden.out 2>&1 || { tail -40 /tmp/rfe_cc_golden.out; exit 1; }
grep -v BuildV2 /tmp/rfe_cc_golden.out
grep -q "RFE CC GOLDEN: OK" /tmp/rfe_cc_golden.out || { echo "RFE CC golden FAILED"; exit 1; }
echo "=== RFE CC golden: OK ==="

echo "=== RFE CC parity (Etapas 2-6: F8a + F8b + F8d + F8e + F8f) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    -I Source/tests Source/tests/test_rfe_cc_parity.cpp -o /tmp/test_rfe_cc_parity || exit 1
/tmp/test_rfe_cc_parity > /tmp/rfe_cc_par.out 2>&1 || { tail -40 /tmp/rfe_cc_par.out; exit 1; }
grep -v BuildV2 /tmp/rfe_cc_par.out
grep -q "RFE CC PARITY (Etapas 2-6): OK" /tmp/rfe_cc_par.out || { echo "RFE CC parity FAILED"; exit 1; }
echo "=== RFE CC parity: OK ==="

echo "=== RFE self-off0 gate (H19 option (a): self present at ANY position) ==="
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    -I Source/tests Source/tests/test_rfe_selfoff0.cpp -o /tmp/test_rfe_selfoff0 || exit 1
/tmp/test_rfe_selfoff0 > /tmp/rfe_selfoff0.out 2>&1 || { tail -40 /tmp/rfe_selfoff0.out; exit 1; }
grep -v BuildV2 /tmp/rfe_selfoff0.out
grep -q "RFE SELF-OFF0 (H19): OK" /tmp/rfe_selfoff0.out || { echo "RFE self-off0 FAILED"; exit 1; }
echo "=== RFE self-off0 gate: OK ==="
