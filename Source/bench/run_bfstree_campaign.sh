#!/bin/bash
# run_bfstree_campaign.sh -- Etapa 2-4 driver: one campaign POINT per invocation.
# Usage: bash run_bfstree_campaign.sh <idx>     (0..3 -> POINTS below)
#   0-2  synthetic bit-exactness / B-sweep points (dense golden A affordable)
#   3    REAL config (ALOI-like L=1400): golden A / ref / E are prohibitive at
#        L=1400 (N^2*L dot products), so FAST mode runs only the production
#        chain A2 -> A3 -> A4. Bit-exactness there is certified by the CK chain
#        (A2's equality with golden was proven at points 0-2 / N=500 / N=10k).
# Under OMP_WAIT_POLICY=PASSIVE (SMT lesson from RDPAC) and appends key=value
# lines to /tmp/bfstree_campaign_<idx>.log. Exit != 0 on any variant failure.
set -u
BIN=/tmp/bench_bfstree
IDX="${1:-0}"
case "$IDX" in
  0) N=5000;  L=100;  K=20 ;;  # saturated regime (k^4/N = 32)
  1) N=10000; L=100;  K=12 ;;  # ALOI-matched regime (k^4/N = 2.07 ~ 2.2)
  2) N=5000;  L=100;  K=10 ;;  # ALOI-matched cheap point (k^4/N = 2.0)
  3) N=13000; L=1400; K=20 ;;  # REAL config (PARAM_BFSTREE_L=1400): F7-bound
  *) echo "bad idx"; exit 2 ;;
esac
# FAST = real-config regime: the dense legacy pipeline is O(N^2 L) and would
# take hours; skip golden/ref/E modes and the wide B sweep (B=256 is the
# measured optimum for L=1400; B=1024 kept as a sanity check).
FAST=0; (( L > 500 )) && FAST=1
LOG=/tmp/bfstree_campaign_$IDX.log
REF=/tmp/bfstree_ref_$IDX.bin
: > "$LOG"
rc=0
tag() { echo "POINT=$IDX N=$N L=$L k=$K $1" >> "$LOG"; }

run() {  # run <mode> <B> <limit> [ref]
  tag "RUN=$1"
  if ! env OMP_WAIT_POLICY=PASSIVE "$BIN" "$1" "$N" "$L" "$K" 42 "$2" "$3" \
       "${4:-}" >> "$LOG" 2>&1; then
    echo "FAILED: $1 (idx=$IDX)"; rc=1
  fi
}

if (( ! FAST )); then
  run ref 0 "$N" "$REF"
  run A   0 "$N" "$REF"
fi
# In FAST mode there is no golden ref file (generating it is prohibitive), so
# pass an empty path: the bench then reports CKs only, no quality comparison.
REFA="$REF"; (( FAST )) && REFA=""
if (( FAST )); then BSWEEP="256 1024"; else BSWEEP="256 1024 4096"; fi
for B in $BSWEEP; do
  run A2 "$B" "$N" "$REFA"
  # A3 = production candidate: A2 + bit-exact parallel colsum, F6 division
  #      FUSED into F7 reads (no N^2 division pass). Must match A2 CK_FINAL.
  run A3 "$B" "$N" "$REFA"
  # A4 = A3 + bit-exact F7 locality rewrite (row-i hoist + ascending-column
  # gather): ~-20% F7 at L=1400. Best production stack. CK must equal A3's.
  run A4 "$B" "$N" "$REFA"
done
if (( ! FAST )); then
  run A3a 256 "$N" "$REF"   # research knob: algebraic O(N k^2) colsum (fp-order
                            # noise only, F6a-gated; NOT bit-exact)
fi
run F6a 0 0               # validation gate: alg colsum vs legacy (BEYOND==0)
if (( ! FAST )); then
  for lim in "$N" "$((8*L))" "$((2*L))" "$L" "$K"; do
    run E 0 "$lim" "$REF"
  done
  run Eattr 0 "$N"
  run Eattr 0 "$L"
fi
echo "CAMPAIGN_POINT_${IDX}_DONE rc=$rc" >> "$LOG"
echo "CAMPAIGN_POINT_${IDX} rc=$rc"
exit $rc
