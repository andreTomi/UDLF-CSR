#!/usr/bin/env bash
# step2_asan.sh -- sanitiser + corner gate for the RFE port (checklist items 4
# and 6), COMMITTABLE -- not ad hoc.
#
# _p2_asan.sh must NOT be reused: it cds to ANOTHER repo
# (/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR) and builds bench_phase.
# This script is the RFE equivalent, with the three runs in one invocation
# (durability rule: one invocation that builds, runs and reports).
#
#   [1] corner under the BENCH flags -- the RESULT line (0 failures) is the gate;
#   [2] parity fixture under ASan+UBSan -- verdict must be OK AND the log must
#       carry no sanitizer report (a sanitizer hit fails the gate even when the
#       digits are right: correct output does not make an OOB read acceptable);
#   [3] corner under ASan+UBSan -- same rule.
#
# FP posture matches step2_check.sh (-O2 -ffp-contract=off, no fast-math) so the
# parity verdict stays comparable; UBSan is fail-closed
# (-fno-sanitize-recover=all). Logs land in $OUT (default /tmp/step2_asan).
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-/tmp/step2_asan}
INC="-I Source/src -I Source/src/Core/hpp -I Source/tests"
FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g -fno-omit-frame-pointer"
mkdir -p "$OUT" || exit 1
fail() { echo "RFE STEP2-ASAN FAILED: $*"; exit 1; }

san_hit() { # any sanitizer report in the log is a hit, whatever the verdict
  grep -qE 'ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY: (Address|Leak|Undefined)Sanitizer' "$1"
}
show_san() {
  grep -aE 'ERROR: |runtime error:|SUMMARY:' "$1" | head -10 | sed 's/^/    /'
}

echo "[1/3] corner sob BENCH (a linha de resultado e o gate, nao um md5)"
$CXX $FLAGS $INC Source/tests/test_rfe_corner.cpp -o "$OUT/corner_bench" 2>/dev/null \
  || fail "build corner_bench"
"$OUT/corner_bench" > "$OUT/corner_bench.log" 2>&1
rc=$?
grep -E 'RFE CORNER:' "$OUT/corner_bench.log" | sed 's/^/    /'
[ "$rc" = 0 ] || fail "corner sob BENCH rc=$rc (ver $OUT/corner_bench.log)"
grep -qE 'RFE CORNER: [0-9]+ checks, 0 failures' "$OUT/corner_bench.log" \
  || fail "corner BENCH nao fechou 0 failures"

echo "[2/3] fixture de paridade sob ASan+UBSan"
$CXX $FLAGS $SAN $INC Source/tests/test_rfe_parity.cpp -o "$OUT/parity_asan" 2>/dev/null \
  || fail "build parity_asan"
OMP_NUM_THREADS=2 ASAN_OPTIONS=detect_leaks=0 "$OUT/parity_asan" \
    > "$OUT/parity_asan.log" 2>&1
rc=$?
grep -E 'RFE PARITY' "$OUT/parity_asan.log" | sed 's/^/    /'
if san_hit "$OUT/parity_asan.log"; then
  show_san "$OUT/parity_asan.log"
  fail "sanitizer report na paridade (ver $OUT/parity_asan.log)"
fi
grep -q '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: OK' "$OUT/parity_asan.log" \
  || fail "paridade sob sanitizers nao fechou OK (rc=$rc; ver $OUT/parity_asan.log)"
grep -q 'E2E Gram (faithful):' "$OUT/parity_asan.log" \
  || fail "bloco E2E Gram ausente no build sanitizado (verifique tmp_fase2.py)"

echo "[3/3] corner sob ASan+UBSan"
$CXX $FLAGS $SAN $INC Source/tests/test_rfe_corner.cpp -o "$OUT/corner_asan" 2>/dev/null \
  || fail "build corner_asan"
OMP_NUM_THREADS=2 ASAN_OPTIONS=detect_leaks=0 "$OUT/corner_asan" \
    > "$OUT/corner_asan.log" 2>&1
rc=$?
grep -E 'RFE CORNER:' "$OUT/corner_asan.log" | sed 's/^/    /'
if san_hit "$OUT/corner_asan.log"; then
  show_san "$OUT/corner_asan.log"
  fail "sanitizer report no corner (ver $OUT/corner_asan.log)"
fi
[ "$rc" = 0 ] || fail "corner sob sanitizers rc=$rc (ver $OUT/corner_asan.log)"
grep -qE 'RFE CORNER: [0-9]+ checks, 0 failures' "$OUT/corner_asan.log" \
  || fail "corner sanitizado nao fechou 0 failures"

echo "STEP2-ASAN: OK -- corner 0/0 em BENCH e sob ASan+UBSan, paridade OK sem relatorio de sanitizer"
