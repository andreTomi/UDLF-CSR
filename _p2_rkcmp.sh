#!/bin/bash
# _p2_rkcmp.sh -- (b) strongest equivalence proof for S2.3 p2: dump the FINAL
# reranked lists of BOTH arms with PHASE_RK_OUT (REPS=1; each rep restores
# rk0 and recomputes deterministically, and the REP checksum sweep in
# _p2_cksum.sh already pins reps together), then compare md5/bytes/cmp and
# run rk_cmp.py per pair.
# Protocols matches _p2_ab.sh: taskset mask, PHASE_THREADS, same dataset.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
D=$R/Data/phase_profile/etapa_b/s23p2
PREBIN=/home/andre/udlf_bin/bp_pre_p2
NEWBIN=/home/andre/udlf_bin/bp_s23
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
OUT=/home/andre/udlf_cache/p2_rk
N=72000; L=400; K=10; LIMIT=400
mkdir -p "$OUT"
cd "$R"

run() { # <bin> <tag> <mask> <T> <pipe>
  taskset -c "$3" env OMP_NUM_THREADS="$4" PHASE_THREADS="$4" \
    PHASE_RK_OUT="$OUT/$2.rk" \
    "$1" real "$BIN" $N $L $K $LIMIT "$5" 1 > "$D/rkcmp_$2.log" 2>&1
}

echo "=== RKOUT dumps (REPS=1) ==="
run "$PREBIN" lhrr12_pre 0-11 12 lhrr
run "$NEWBIN" lhrr12_new 0-11 12 lhrr
run "$PREBIN" lhrr1_pre  0    1  lhrr
run "$NEWBIN" lhrr1_new  0    1  lhrr
run "$PREBIN" cprr12_pre 0-11 12 cprr
run "$NEWBIN" cprr12_new 0-11 12 cprr
grep -aH '^RKOUT,' "$D"/rkcmp_*.log || { echo "FATAL: RKOUT line missing"; exit 2; }

echo
echo "=== size gate (must be N*L*4=$((N * L * 4))) ==="
bad=0
for f in "$OUT"/*.rk; do
  s=$(stat -c%s "$f")
  [ "$s" -eq $((N * L * 4)) ] || { echo "SIZE FAIL $f ($s)"; bad=1; }
done
[ "$bad" -eq 0 ] && echo "all sizes OK"

echo
echo "=== md5 ==="
md5sum "$OUT"/*.rk

echo
echo "=== pairwise cmp, pre vs new (gate) ==="
rc=0
for p in lhrr12 lhrr1 cprr12; do
  if cmp -s "$OUT/${p}_pre.rk" "$OUT/${p}_new.rk"; then
    echo "IDENTICAL $p"
  else
    echo "DIFF $p"; rc=1
  fi
done

echo
echo "=== info: cross-T / cross-pipe (not gated) ==="
for pair in "lhrr12_new lhrr1_new" "lhrr12_pre lhrr1_pre"; do
  set -- $pair
  if cmp -s "$OUT/$1.rk" "$OUT/$2.rk"; then echo "IDENTICAL $1 == $2"; else echo "DIFF $1 != $2"; fi
done

echo
echo "=== rk_cmp.py per gated pair ==="
for p in lhrr12 lhrr1 cprr12; do
  echo "-- $p --"
  python3 "$R/Source/bench/rk_cmp.py" "$OUT/${p}_pre.rk" "$OUT/${p}_new.rk" $N $L || rc=1
done

echo
if [ "$rc" -eq 0 ]; then echo "GATE rkcmp: PASS (all pre==new bit-equal)"; else echo "GATE rkcmp: FAIL"; fi
exit $rc
