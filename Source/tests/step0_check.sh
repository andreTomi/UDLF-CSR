#!/usr/bin/env bash
# step0_check.sh -- determinism + golden capture for the RFE Step 0 harness.
#
# Read-only w.r.t. the repo: builds /tmp/test_rfe_golden from Source/tests only.
# Nothing here touches Source/src or Source/bench.
#
# The thread loop is here (not in a -c one-liner) because the PowerShell->WSL
# bridge eats "$th" and "$p" in single-quoted -c strings.
set -u

# ROOT may be pre-set by the caller (needed when this file is executed from the
# CRLF-stripped copy under /tmp, whose dirname-relative path is not the repo).
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1

CXX=${CXX:-g++}
BIN=${BIN:-/tmp/test_rfe_golden}
# -O2 -ffp-contract=off: the house strict-IEEE posture for gates (BFSTree/RDPAC
# precedent). These pins are an -O2 contract; survival under the product flags
# is a Step 4 measurement, not assumed here.
FLAGS=${FLAGS:-"-std=gnu++2b -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter"}

fail() { echo "RFE STEP0 FAILED: $*" >&2; exit 1; }

echo "[build] test_rfe_golden"
$CXX $FLAGS -I Source/tests Source/tests/test_rfe_golden.cpp -o "$BIN" \
  || fail "build"

echo "[1/3] invariants + census"
"$BIN" | tail -n +1 > /tmp/rfe_step0.out || fail "execucao (rc!=0)"
grep -q '^RFE GOLDEN: OK$' /tmp/rfe_step0.out \
  || { cat /tmp/rfe_step0.out; fail "golden reportou falha"; }

echo "[2/3] determinismo entre 3 execucoes (mesmo binario, sem batch)"
a=$("$BIN" | md5sum | cut -d' ' -f1)
b=$("$BIN" | md5sum | cut -d' ' -f1)
c=$("$BIN" | md5sum | cut -d' ' -f1)
[ "$a" = "$b" ] && [ "$b" = "$c" ] \
  || fail "nao deterministico entre execucoes ($a $b $c)"

echo "[3/3] determinismo entre contagens de thread (OMP)"
m=""
for th in 1 2 4 12; do
  h=$(OMP_NUM_THREADS=$th "$BIN" | md5sum | cut -d' ' -f1)
  m="$m $h"
done
u=$(printf '%s\n' $m | sort -u | wc -l)
[ "$u" = 1 ] || fail "nao deterministico entre threads ($m)"

echo "      md5 identico em 3 execucoes e em 1/2/4/12 threads ($a)"

# ---------------------------------------------------------------------------
# Sweep: is the census a property of the METHOD or of the pinned fixture?
# Briefing rule 7 -- do not generalise from a single fixture. Each row varies one
# knob and re-records the census, so a claim that only holds at N=200/k=10 shows
# up here instead of surviving until Step 2.
#
# The k column is the important one. The plan predicted a STRUCTURAL CLIFF: the
# smallest nonzero rankKScore is r_min = 1 - log(k-1)/log(k), and after
# combineReverseEmb a legitimate nonzero emb value can be as small as r_min^4.
# The CSR engine's multiply_and_prune drops |v| <= 1e-9 on emission
# (SparseMatrix.hpp:559), so beyond the k where r_min^4 <= 1e-9 the engine would
# discard REAL nonzeros. Solving r_min^4 = 1e-9 predicts that cliff near
# k ~ 46-50. This sweep CANNOT prove the cliff (it measures the legacy, not the
# port) -- what it does is pin the census across k so Step 1 has the numbers,
# and it verifies the legacy does not itself degenerate as k grows.
# ---------------------------------------------------------------------------
echo
echo "-- sweep: census vs fixture (the pinned row is N=200 L=40 k=10) --"
printf '%-5s %-5s %-5s %-5s %-8s %-10s %-10s %-9s %-10s %-8s %s\n' \
  N L k t emb_keys emb_zeros emb_norev zero_frac inc_total inc/L verdict
sweep_fail=0
for cfg in "200 40 10 2" "200 40 10 1" "200 40 10 3" \
           "200 40 5 2"  "200 40 20 2" "200 40 40 2" "200 80 40 2" \
           "200 120 60 2" "100 40 10 2" "400 40 10 2" \
           "50 20 8 2"  "200 40 39 2" "200 120 49 2" \
           "200 200 100 1" "200 200 20 2"; do
  set -- $cfg
  n=$1; ll=$2; kk=$3; tt=$4
  # Deliberately NOT `out=$(...) || fail`: the binary exits non-zero when an
  # assertion fails, which is exactly the L==N case we want to CLASSIFY as SAT,
  # not to report as "did not run". An earlier version used `||` and every
  # saturated row printed NAO RODOU, hiding the boundary it was meant to expose.
  # set -u is on but set -e is not, so a non-zero rc here is just recorded.
  out=$("$BIN" "$n" "$ll" "$kk" "$tt" 2>&1); rc=$?
  last=$(printf '%s\n' "$out" | grep '^cartesianProduct' | tail -1)
  if [ -z "$last" ]; then
    echo "sweep N=$n L=$ll k=$kk t=$tt: sem linha cartesianProduct (rc=$rc)"
    sweep_fail=1
    continue
  fi
  keys=$(printf '%s\n' "$last" | sed 's/.*emb_keys=\([0-9]*\).*/\1/')
  zeros=$(printf '%s\n' "$last" | sed 's/.*emb_zeros=\([0-9]*\).*/\1/')
  norev=$(printf '%s\n' "$last" | sed 's/.*emb_norev=\([0-9]*\).*/\1/')
  inc=$(printf '%s\n' "$last" | sed 's/.*inc_total=\([0-9]*\).*/\1/')
  frac=$(awk -v z="$zeros" -v a="$keys" 'BEGIN{ if(a>0) printf "%.4f", z/a; else print "0" }')
  perrow=$(awk -v i="$inc" -v n="$n" 'BEGIN{ if(n>0) printf "%.1f", i/n; else print "0" }')
  v=$(printf '%s\n' "$out" | grep -c '^RFE GOLDEN: OK$' || true)
  # The L==N rows CANNOT satisfy the discovery assertions: the fixture row is a
  # permutation of [0,N) cut to L, so at L==N it already contains every image and
  # the Gram has nothing to find (measured: gained == 0, inc_total == N*L).
  # See step0_probe_saturated.sh. Expected-not-ok there, so it is recorded as
  # SAT rather than counted as a failure -- an unexplained failure is never
  # tolerated, but an explained, measured boundary is.
  if [ "$v" = 1 ]; then
    verdict=OK
  elif [ "$ll" -ge "$n" ]; then
    verdict=SAT
  else
    verdict=CHECK
    sweep_fail=1
  fi
  printf '%-5s %-5s %-5s %-5s %-8s %-10s %-10s %-9s %-10s %-8s %s\n' \
    "$n" "$ll" "$kk" "$tt" "$keys" "$zeros" "$norev" "$frac" "$inc" "$perrow" "$verdict"
done
[ "$sweep_fail" = 0 ] || fail "sweep: pelo menos uma configuracao nao-L==N nao fechou OK"
echo "      todas as configuracoes nao-saturadas fecharam OK; linhas SAT = L==N,"
echo "      limite medido em que a descoberta de candidatos desaparece (probe)."

echo
echo "-- golden (canonical) --"
"$BIN" --record | awk '/^-- golden/,0'
echo
echo "RFE STEP0: OK"