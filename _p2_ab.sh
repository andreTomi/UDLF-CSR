#!/bin/bash
# _p2_ab.sh <ab12|ab1|abcprr> -- same-session A/B, pre-removal vs post-removal.
# pre = commit 1f56845 exported to /home/andre/pre_p2_src and built with the
# SAME flags as bp_s23 (in particular -DNDEBUG, so asserts do not bias the A/B).
# ABBA order (pre,new | new,pre) so a monotone drift cancels; warm-ups discarded.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
D=$R/Data/phase_profile/etapa_b/s23p2
PREBIN=/home/andre/udlf_bin/bp_pre_p2
NEWBIN=/home/andre/udlf_bin/bp_s23
SRC=/home/andre/pre_p2_src
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
N=72000; L=400; K=10; LIMIT=400; REPS=3
MODE=${1:?usage: _p2_ab.sh <ab12|ab1|abcprr>}

if [ ! -x "$PREBIN" ]; then
  echo "=== building $PREBIN from commit 1f56845 ==="
  mkdir -p "$SRC"
  ( cd "$R" && git archive 1f56845 | tar -x -C "$SRC" )
  g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -DPHASE_PROFILE -I "$SRC/Source/src" \
      "$SRC/Source/bench/bench_phase.cpp" -o "$PREBIN" -ltbb
  echo "PRE build OK -> $PREBIN"
fi

case "$MODE" in
  ab12)   MASK=0-11; T=12; PIPE=lhrr ;;
  ab1)    MASK=0;    T=1;  PIPE=lhrr ;;
  abcprr) MASK=0-11; T=12; PIPE=cprr ;;
  *) echo "bad mode"; exit 2 ;;
esac
mkdir -p "$D/raw_$MODE"
cd "$R"

run_arm() {  # <bin> <tag>
  taskset -c "$MASK" env OMP_NUM_THREADS=$T PHASE_THREADS=$T \
    "$1" real "$BIN" $N $L $K $LIMIT "$PIPE" $REPS > "$D/raw_$MODE/$2.txt" 2>&1
}

echo "=== warm-ups (discarded) ==="
run_arm "$PREBIN" warm_pre
run_arm "$NEWBIN" warm_new

echo "=== ABBA pairs (REPS=$REPS, T=$T, $PIPE) ==="
run_arm "$PREBIN" r1_pre
run_arm "$NEWBIN" r1_new
run_arm "$NEWBIN" r2_new
run_arm "$PREBIN" r2_pre
if [ "$MODE" = "ab12" ]; then
  run_arm "$PREBIN" r3_pre
  run_arm "$NEWBIN" r3_new
fi

for t in r1_pre r1_new r2_new r2_pre r3_pre r3_new; do
  [ -f "$D/raw_$MODE/$t.txt" ] || continue
  echo "## $t"
  grep -a -E '^DRVAGG,|^REP,|^DONE,' "$D/raw_$MODE/$t.txt"
done
echo "=== AB MODE $MODE DONE ==="
