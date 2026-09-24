#!/bin/bash
# _fase1_rkcmp.sh -- Passo D (Fase 1 S2.4): prova forte de equivalencia pos-
# fast-path: PHASE_RK_OUT das listas finais reranqueadas, bp_fase0 (pre) vs
# bp_s23 (Fase 1), md5/cmp identicos nos 3 pares. Protocolo = _p2_rkcmp.sh.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
D=$R/Data/phase_profile/etapa_b/s24
PREBIN=/home/andre/udlf_bin/bp_fase0
NEWBIN=/home/andre/udlf_bin/bp_s23
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
OUT=/home/andre/udlf_cache/f1_rk
N=72000; L=400; K=10; LIMIT=400
mkdir -p "$OUT" "$D"
cd "$R"

run() { # <bin> <tag> <mask> <T> <pipe>
  taskset -c "$3" env OMP_NUM_THREADS="$4" PHASE_THREADS="$4" \
    PHASE_RK_OUT="$OUT/$2.rk" \
    "$1" real "$BIN" $N $L $K $LIMIT "$5" 1 > "$D/rkcmp_$2.log" 2>&1
}

echo "=== FASE1 RKOUT dumps (REPS=1) ==="
run "$PREBIN" f0_lhrr12 0-11 12 lhrr
run "$NEWBIN" f1_lhrr12 0-11 12 lhrr
run "$PREBIN" f0_lhrr1  0    1  lhrr
run "$NEWBIN" f1_lhrr1  0    1  lhrr
run "$PREBIN" f0_cprr12 0-11 12 cprr
run "$NEWBIN" f1_cprr12 0-11 12 cprr
grep -aH '^RKOUT,' "$D"/rkcmp_f*.log || { echo "FATAL: RKOUT line missing"; exit 2; }

echo
echo "=== size gate (must be N*L*4=$((N * L * 4))) ==="
bad=0
for f in "$OUT"/*.rk; do
  s=$(stat -c%s "$f")
  [ "$s" -eq $((N * L * 4)) ] || { echo "SIZE FAIL $f ($s)"; bad=1; }
done
[ "$bad" -eq 0 ] && echo "all sizes OK"

echo
echo "=== pairwise cmp, fase0 vs fase1 (GATE) ==="
rc=0
for p in lhrr12 lhrr1 cprr12; do
  if cmp -s "$OUT/f0_${p}.rk" "$OUT/f1_${p}.rk"; then
    echo "IDENTICAL $p"
  else
    echo "DIFF $p"; rc=1
  fi
done
md5sum "$OUT"/*.rk
[ "$rc" -eq 0 ] && echo "FASE1 RK GATE: PASS" || echo "FASE1 RK GATE: FAIL"
exit $rc