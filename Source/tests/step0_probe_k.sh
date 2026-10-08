#!/usr/bin/env bash
# step0_probe_k.sh -- why does the exact-zero census collapse as k grows?
#
# Step 0's sweep produced a result that CONTRADICTS the plan's structural
# prediction, so it gets its own probe rather than being quietly folded into the
# sweep table. The rule: a negative or surprising finding gets measured and
# written down, never hidden or adjusted until it agrees with the plan.
#
# The prediction was: emb carries keys whose value is EXACTLY 0.0f, because
# rankKScore(k) = 1 - log(k)/log(k) = 0 exactly (RFE.cpp:645) and incEmb INSERTS
# the key (RFE.cpp:692). Measured at N=200 L=40 k=10 the census showed
# zeros=7910 of 12812 keys (62%), which supported the prediction.
#
# But the sweep shows the zero FRACTION collapsing as k grows:
#   k=5  -> 71.7%     k=20 -> 24.1%     k=40 -> 0.08%
# and at k=40 the census finds essentially NO zeros, which FAILED the golden's
# own assertion. The mechanism is support SATURATION, not arithmetic: as k grows
# toward L, the k-hop fan-out of emb[q] covers more of [0,N), so the SAME column
# receives contributions from many pos1 values. The exact zero contributed by
# pos1 = k-1 is then ADDED to other nonzero terms, and `+=` of 0.0f is a no-op --
# the zero stops being visible in the final value even though the key was
# inserted. The insertion still happens; it just stops surviving to the census.
#
# This is exactly the briefing's rule 2 (synthetic density lies): at N=200 with
# L=40 the id space is small enough that a 40-wide fan-out nearly saturates it.
# The REAL question -- whether zeros are load-bearing at production k on real
# data (ALOI N=72k, L=400, k=20) -- is NOT answered here and must be answered in
# Step 3 on real data. What this probe establishes is that the census is
# REGIME-DEPENDENT, so the golden's assertion must be scoped, not universal.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
# Builds its own binary (distinct name to avoid colliding with step0_check.sh's).
# An earlier version assumed /tmp/test_rfe_golden already existed and failed with
# "build ... first" whenever the probes were run before the gate -- ordering
# dependence between gates is exactly the kind of thing that silently rots.
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/test_rfe_golden_pk}
FLAGS=${FLAGS:-"-std=gnu++2b -O2 -ffp-contract=off"}

$CXX $FLAGS -I Source/tests Source/tests/test_rfe_golden.cpp -o "$BIN" || exit 1

echo "-- zero-fraction of emb vs k (N=200 L=40; N^2=40000 cells) --"
printf '%-4s %-10s %-10s %-11s %-10s %s\n' k emb_keys emb_zeros zero_frac sat_ratio note
for kk in 3 5 10 15 20 25 30 35 40; do
  line=$("$BIN" 200 40 "$kk" 2 2>&1 | grep '^embRR\[1\]' | tail -1)
  [ -n "$line" ] || { printf '%-4s %s\n' "$kk" "NAO RODOU"; continue; }
  keys=$(printf '%s\n' "$line" | sed 's/.*emb_keys=\([0-9]*\).*/\1/')
  zeros=$(printf '%s\n' "$line" | sed 's/.*emb_zeros=\([0-9]*\).*/\1/')
  frac=$(awk -v z="$zeros" -v a="$keys" 'BEGIN{ if(a>0) printf "%.4f", z/a; else print "0" }')
  sat=$(awk -v a="$keys" 'BEGIN{ printf "%.3f", a/40000 }')
  note=""
  awk -v z="$zeros" 'BEGIN{ exit !(z==0) }' && note="SEM ZEROS"
  printf '%-4s %-10s %-10s %-11s %-10s %s\n' "$kk" "$keys" "$zeros" "$frac" "$sat" "$note"
done

echo
echo "-- leitura: a fracao de zeros cai porque o SUPORTE satura, nao porque"
echo "   a aritmetica muda. A chave continua sendo inserida; o 0.0f exato deixa"
echo "   de sobreviver a soma porque outros pos1 ja escreveram nela."