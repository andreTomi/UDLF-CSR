#!/usr/bin/env bash
# step1_probe_h8.sh -- why does H8 hold at the pinned fixture and fail elsewhere?
#
# H8 is the load-bearing claim of the whole port: it is what licenses pruning the
# Gram to top-L instead of accumulating it. Measured at the pinned fixture
# (N=120 L=30 k=8) it holds with 0 mismatched rows. In the sweep it FAILS on
# 5 of 7 fixtures -- including N=200 L=40 k=10, which differs from the pin only
# in size. A lemma that survives one fixture and dies on the next is not a
# lemma, so this probe isolates WHICH part of the key is wrong.
#
# Candidate causes, in the order they should be tested:
#   (A) the current list used for membership/priority is the wrong one (the pin
#       passed, the sweep failed -> probably NOT this, but it is the cheapest to
#       rule out);
#   (B) the tie key is incomplete: the legacy's insertion sort is stable over
#       rkTmp = [current list in order] ++ [extras ascending], but kernelSortRankedLists_extra
#       also compares against cells the Gram never touched, which read the
#       ZERO-FILL of `matrix`. Those read 0.0f and are neither in the Gram nor
#       in the current list -- and 0.0f loses every tie to a current-list member
#       that is also 0.0f, but BEATS every extra with a negative value;
#   (C) the diagonal pin pow(2,30) makes the self rank first regardless, and my
#       rebuild reproduces that, but the legacy ALSO includes cells whose Gram
#       value is 0 in the sort.
#
# (B) and (C) are the same root cause in different clothes: the legacy sorts over
# the UNION of (current list) and (discovered candidates), where a candidate is
# "discovered" by incElems -- which includes pairs whose PRODUCT IS EXACTLY ZERO.
# H2 already measured that ~20% of emb keys are exactly zero, and incSimByECP
# inserts those into incElems unconditionally (RFE.cpp:710). So incElems is
# strictly larger than the set of cells with a nonzero Gram value, and pruning
# "the Gram to top-L by value" loses those zero-valued candidates, which the
# legacy still considered.
#
# This probe prints, for the first mismatching row: |incElems[q]| vs the number
# of nonzero Gram cells in row q, and how many of the legacy's kept extras are
# zero-valued. That last number is the hypothesis test for (B)/(C).
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/step1_h8probe}
FLAGS=${FLAGS:-"-std=gnu++2b -O2 -ffp-contract=off -fopenmp"}

$CXX $FLAGS -I Source/src -I Source/src/Core/hpp -I Source/tests \
  Source/tests/test_rfe_hyp.cpp -o "$BIN" || exit 1

for cfg in "120 30 8 2" "150 50 25 2" "200 40 10 2"; do
  echo "=================== fixture: $cfg"
  OMP_NUM_THREADS=1 "$BIN" $cfg 2>&1 | grep -v BuildV2 | grep -E "^H[78]|FAIL" || true
done

echo
echo "-- leitura: H8 passa no pin e falha em fixtures MAIORES. O gate exige 0"
echo "   falhas, entao H8 NAO esta validado ate que a causa seja isolada e"
echo "   corrigida. Nao degradar a assercao para tolerancia -- um prune que muda"
echo "   a rank list final nao e um detalhe de ULP."