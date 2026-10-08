#!/usr/bin/env bash
# step0_probe_saturated.sh -- the L=N boundary: candidate discovery VANISHES.
#
# Second surprising Step 0 result, and again it is a property of the REGIME
# rather than of the code. The sweep row N=200 L=200 k=100 failed three
# assertions, all of them about discovery:
#
#   inc_total = 40000 = N*L exactly, and 0 of 40000 final row slots were newly
#   discovered candidates.
#
# The mechanism: the fixture generator emits row q as a permutation of [0,N)
# truncated to L entries. When L == N the row ALREADY CONTAINS EVERY IMAGE, so
# there is nothing outside the rank list for the Gram to find -- incElems[q] can
# only ever be a subset of the row itself, and set_difference against
# incElemsL[q] is empty by construction.
#
# This is worth writing down rather than deleting the sweep row, because it
# bounds WHERE the candidate-discovery machinery matters: it needs L < N with
# room to spare. The production configs do have that (ALOI N=72000 L=400, a
# 180x gap), so discovery is the dominant behaviour there; but it means a
# saturation fixture cannot test the discovery path at all, exactly like the
# saturated fixtures could not test the exact-zero path.
#
# Both Step 0 surprises share one shape: a synthetic fixture that saturates the
# id space cannot exercise the structural claim it looks like it is exercising.
# That is briefing rule 2 (synthetic density lies) biting inside Step 0 itself,
# and it is the reason the golden's assertions are regime-scoped rather than
# universal.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/test_rfe_golden_sat}
FLAGS=${FLAGS:-"-std=gnu++2b -O2 -ffp-contract=off"}

$CXX $FLAGS -I Source/tests Source/tests/test_rfe_golden.cpp -o "$BIN" || exit 1

echo "-- discovery vs L/N ratio (N=200; rows are permutations of [0,N) cut to L) --"
printf '%-5s %-6s %-8s %-10s %-10s %-10s %s\n' L L_over_N inc_total N_times_L gained verdict
for ll in 20 40 80 120 160 200; do
  out=$("$BIN" 200 "$ll" 20 2 2>&1)
  last=$(printf '%s\n' "$out" | grep '^cartesianProduct' | tail -1)
  [ -n "$last" ] || { printf '%-5s NAO RODOU\n' "$ll"; continue; }
  inc=$(printf '%s\n' "$last" | sed 's/.*inc_total=\([0-9]*\).*/\1/')
  gained=$(printf '%s\n' "$out" | sed -n 's/.*info: \([0-9]*\) of .*newly discovered.*/\1/p' | tail -1)
  [ -n "$gained" ] || gained=0
  ratio=$(awk -v l="$ll" 'BEGIN{ printf "%.3f", l/200 }')
  nl=$((200 * ll))
  v=$(printf '%s\n' "$out" | grep -c '^RFE GOLDEN: OK$' || true)
  verdict=$([ "$v" = 1 ] && echo OK || echo CHECK)
  printf '%-5s %-6s %-8s %-10s %-10s %s\n' "$ll" "$ratio" "$inc" "$nl" "$gained" "$verdict"
done

echo
echo "-- leitura: quando L -> N a lista ja contem TODAS as imagens, o Gram nao tem"
echo "   nada a descobrir (inc_total == N*L, gained == 0). Descoberta de"
echo "   candidatos so tem regime onde L << N; ALOI (N=72000 L=400) tem folga 180x."