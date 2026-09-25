#!/bin/bash
# run_bfstree_campaign.sh -- Etapa 2 driver: one campaign POINT per invocation.
# Usage: bash run_bfstree_campaign.sh <idx>     (0,1,2 -> POINTS below)
# Runs ref / A / A2(B sweep) / E(limit sweep) / Eattr under
# OMP_WAIT_POLICY=PASSIVE (SMT lesson from RDPAC) and appends key=value lines
# to /tmp/bfstree_campaign_<idx>.log. Exit != 0 on any variant failure.
set -u
BIN=/tmp/bench_bfstree
IDX="${1:-0}"
case "$IDX" in
  0) N=5000; L=100; K=20 ;;   # saturated regime (k^4/N = 32)
  1) N=10000; L=100; K=12 ;;  # ALOI-matched regime (k^4/N = 2.07 ~ 2.2)
  2) N=5000; L=100; K=10 ;;   # ALOI-matched cheap point (k^4/N = 2.0)
  *) echo "bad idx"; exit 2 ;;
esac
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

run ref 0 "$N" "$REF"
run A   0 "$N" "$REF"
for B in 256 1024 4096; do
  run A2 "$B" "$N" "$REF"
done
for lim in "$N" "$((8*L))" "$((2*L))" "$L" "$K"; do
  run E 0 "$lim" "$REF"
done
run Eattr 0 "$N"
run Eattr 0 "$L"
echo "CAMPAIGN_POINT_${IDX}_DONE rc=$rc" >> "$LOG"
echo "CAMPAIGN_POINT_${IDX} rc=$rc"
exit $rc
