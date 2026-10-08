#!/usr/bin/env bash
# step2_check.sh -- gate for the RFE port (Step 2). Phases F1..F7 at this revision.
#
# Same posture as the other gates: -O2, -ffp-contract=off, no fast-math.
# Additive: touches no md5 registry, no existing gate, nothing under Source/src
# beyond the file being tested.
#
# Four things are required, in order of strictness:
#   [1] the TU check compiles (a header that only compiles when included the way
#       the product includes it);
#   [2] the PINNED fixture reproduces the Step 0 pins bit-for-bit;
#   [3] the md5 is stable across 1/2/4/12 threads (the core is parallel, so a
#       thread-dependent digest would mean non-determinism leaked in);
#   [4] a sweep of other fixtures closes with 0 failures -- the pin proves one
#       point, the sweep is what stops it being believed as a general claim.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1

CXX=${CXX:-g++}
BIN=${BIN:-/tmp/step2_parity}
TU=${TU:-/tmp/step2_tu}
FLAGS=${FLAGS:-"-std=gnu++2b -O2 -ffp-contract=off -fopenmp"}
INC="-I Source/src -I Source/src/Core/hpp -I Source/tests"

fail() { echo "RFE STEP2 FAILED: $*" >&2; exit 1; }

echo "[1/4] TU check (RFECore.hpp)"
$CXX $FLAGS -I Source/src -I Source/src/Core/hpp Source/tests/check_rfe_tu.cpp -o "$TU" 2>/dev/null \
  || fail "build do check_rfe_tu"
"$TU" 2>&1 | grep -v BuildV2 | sed 's/^/      /' || fail "execucao do TU check"

echo "[2/4] build do gate de paridade"
$CXX $FLAGS $INC Source/tests/test_rfe_parity.cpp -o "$BIN" 2>/dev/null \
  || fail "build do test_rfe_parity"

echo "[3/4] fixture fixo (N=200 L=40 k=10 t=2 seed=42) -- pins do Step 0"
out=$("$BIN" 2>&1 | grep -v BuildV2) || fail "execucao"
printf '%s\n' "$out" | grep -E '^(  legacy|  core|  ok|  exact-zero|-- F|RFE PARITY)'
printf '%s\n' "$out" | grep -q '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: OK' \
  || { printf '%s\n' "$out" | grep FAIL; fail "fixture fixo nao fechou OK"; }

echo "[4/4] determinismo entre threads + sweep de fixtures"
m=""
for th in 1 2 4 12; do
  h=$(OMP_NUM_THREADS=$th "$BIN" 2>&1 | grep -v BuildV2 | md5sum | cut -d' ' -f1)
  m="$m $h"
done
u=$(printf '%s\n' $m | sort -u | wc -l)
[ "$u" = 1 ] || fail "core nao deterministico entre threads ($m)"
echo "      md5 identico em 1/2/4/12 threads"

sweep_fail=0
# pr collects the union-prune soundness table: "PRUNE N L k t union_bad valueonly_bad".
# The decision section (FINDINGS.md) will claim the union prune is EXACT, and that
# claim is only allowed to stand if it is 0 on EVERY fixture, not just the pin (X3).
pr_rows=$(mktemp)
: > "$pr_rows"
# grv corroborates FINDINGS.md's "grValue > 0 by construction": the theorem says
# it can never be 0 for any input the core accepts, so a 0 here would refute it.
# It is a COLUMN so the sweep cannot quietly stop reporting it, and a 0 makes the
# gate FAIL rather than print a reassuring table.
grv_zero=0
printf '%-18s %-8s %-9s %-10s %-12s %s\n' fixture result emb_keys zero_keys grValue checks
for cfg in "200 40 10 2" "120 30 8 2" "80 20 6 1" "300 60 15 3" \
           "150 50 25 2" "200 200 20 1" "60 60 30 1" "400 40 10 2" \
           "200 40 10 1" "200 40 5 2"; do
  set -- $cfg
  o=$("$BIN" "$1" "$2" "$3" "$4" 2>&1 | grep -v BuildV2)
  if printf '%s\n' "$o" | grep -q '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: OK'; then r=OK; else r=CHECK; sweep_fail=1; fi
  keys=$(printf '%s\n' "$o" | sed -n 's/.*legacy emb keys=\([0-9]*\).*/\1/p' | tail -1)
  zero=$(printf '%s\n' "$o" | sed -n 's/.*exact-zero keys: legacy=\([0-9]*\).*/\1/p' | tail -1)
  grv=$(printf '%s\n' "$o" | sed -n 's/.*grValue=\([0-9.e+-]*\).*/\1/p' | head -1)
  [ -n "$grv" ] || grv=MISSING
  [ "$grv" = "0" ] && grv_zero=$((grv_zero + 1))
  printf '%s\n' "$o" | grep '^    PRUNE ' >> "$pr_rows"
  nok=$(printf '%s\n' "$o" | grep -c '^  ok' || true)
  nfail=$(printf '%s\n' "$o" | grep -c 'FAIL' || true)
  printf '%-18s %-8s %-9s %-10s %-12s %s ok / %s FAIL\n' "$cfg" "$r" "$keys" "$zero" "$grv" "$nok" "$nfail"
  [ "$r" = OK ] || printf '%s\n' "$o" | grep 'FAIL'
done
[ "$sweep_fail" = 0 ] || fail "sweep: alguma configuracao nao fechou OK"
[ "$grv_zero" -eq 0 ] || fail "grValue == 0 em $grv_zero config(s): o teorema 'grValue > 0 by construction' foi refutado"

# Union-prune soundness across the WHOLE sweep. X3: a soundness claim backed by
# one fixture is not a claim. The gate FAILS if any fixture's union prune is not
# exact, so "the prune is exact" cannot rot into an unchecked assumption.
pr_n=$(wc -l < "$pr_rows")
[ "$pr_n" -eq 10 ] || fail "union-prune: esperado 10 fixtures na tabela PRUNE, veio $pr_n (gate pode ter deixado de reportar)"
pr_bad=$(awk '{s += $6} END {print s+0}' "$pr_rows")
pr_vo=$(awk '{s += $7} END {print s+0}' "$pr_rows")
[ "$pr_bad" -eq 0 ] || fail "union-prune: $pr_bad linhas divergem em algum fixture -- a soundness NAO vale"
printf '      union-prune soundness (%s fixtures): N L k t -> rows_differing (union / value-only)\n' "$pr_n"
sed 's/^    PRUNE /        /' "$pr_rows"
printf '      union-prune: %s linhas divergentes no total (union) | %s (value-only, a alternativa errada)\n' "$pr_bad" "$pr_vo"
echo "      todas as configuracoes fecharam OK; grValue > 0 em todas (teorema corroborado)"

echo
echo "RFE STEP2: OK (F1 rankNorm+sort, F2 createEmb, F3 createReverseEmb, F4 combineReverseEmb, F5 sortEmb+effectEstimationEmb, F6 Gram+incElems, F7 rerank+union-prune -- bit-identical ao legado)"
