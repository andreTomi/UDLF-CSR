#!/bin/bash
# _p2_profile.sh <t12|t1|cprr> -- S2.3 passo 2 end-to-end profile (post-removal).
# Same protocol as s22b_run.sh: taskset mask, PHASE_THREADS, warm-up discarded,
# rep 0 discarded, canary at T=12. Evidence lands in Data/.../s23p2/.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
D=$R/Data/phase_profile/etapa_b
OUT=$D/s23p2
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
DRIVER=/home/andre/udlf_bin/bp_s23
N=72000; L=400; K=10; LIMIT=400; REPS=3
mkdir -p "$OUT/t1/raw" "$OUT/t12/raw" "$OUT/cprr/raw"
cd "$R"

MODE=${1:?usage: _p2_profile.sh <t12|t1|cprr>}

case "$MODE" in
  t12)
    echo "=== T=12 LHRR: warm (discarded) ==="
    taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
      "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/t12/raw/warm.txt" 2>&1
    echo "=== canary_0 ==="
    # fix_crlf copia canary.sh para /tmp (repo e CRLF/9p); fallback ao repo.
    if [ -f /tmp/canary.sh ]; then source /tmp/canary.sh; else source "$R/Source/bench/canary.sh"; fi
    canary_run "$DRIVER" "$OUT/t12/raw/canary_0.txt"
    canary_check "$OUT/t12/raw/canary_0.txt"
    echo "=== measured m1 ==="
    taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
      "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/t12/raw/m1.txt" 2>&1
    echo "=== canary_1 ==="
    canary_run "$DRIVER" "$OUT/t12/raw/canary_1.txt"
    canary_check "$OUT/t12/raw/canary_1.txt"
    ;;
  t1)
    echo "=== T=1 LHRR: warm (discarded) ==="
    taskset -c 0 env OMP_NUM_THREADS=1 PHASE_THREADS=1 \
      "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/t1/raw/warm.txt" 2>&1
    echo "=== measured m1 ==="
    taskset -c 0 env OMP_NUM_THREADS=1 PHASE_THREADS=1 \
      "$DRIVER" real "$BIN" $N $L $K $LIMIT lhrr $REPS > "$OUT/t1/raw/m1.txt" 2>&1
    ;;
  cprr)
    echo "=== T=12 CPRR ==="
    taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 \
      PHASE_RK_OUT="$OUT/cprr/raw/m1.rk.bin" \
      "$DRIVER" real "$BIN" $N $L $K $LIMIT cprr $REPS > "$OUT/cprr/raw/m1.txt" 2>&1
    ;;
  *) echo "bad mode"; exit 2 ;;
esac

echo
echo "############ NEW driver -- $MODE ############"
grep -a -E '^ABKNOBS,|^TSET,|^DRVAGG,|^REP,|^DONE,' "$OUT/$MODE/raw/m1.txt"

echo
echo "############ BASELINE s22b v2 arm -- $MODE ############"
if [ "$MODE" = "cprr" ]; then
  grep -a -E '^DRVAGG,|^DONE,' "$D/s22b/t12/raw/cprr_v2.txt"
else
  grep -a -E '^DRVAGG,|^DONE,' "$D/s22b/$MODE/raw/p2_v2.txt"
fi
echo "=== PROFILE MODE $MODE DONE ==="
