#!/bin/bash
# Regime ladder on the FAITHFUL pipeline (via run()) in real ALOI. The old ladder
# was measured on the short sequence and is superseded. Expectation, not result:
# NFM == BENCH == 0x261f6b6bca514f76.
set -u
cd /mnt/c/Users/andre/Documents/phd/side_projects/UDLF-CSR/UDLF-CSR || exit 1
OUT=Data/phase_profile/rfe_numerics_prod
mkdir -p "$OUT"
ALOI=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
PROD=$(make -C Source -s show-flags 2>/dev/null | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD" ] || { echo "no PROD flags"; exit 1; }
PROD="$PROD -ffast-math -funsafe-math-optimizations -ffinite-math-only"
PRODC="$PROD -ffp-contract=off"
NFM=$(printf '%s\n' "$PROD" | tr -s ' ' | sed -e 's/ -ffast-math//g' -e 's/ -funsafe-math-optimizations//g' -e 's/ -ffinite-math-only//g')
NFM="$NFM -ffp-contract=off"
BENCH="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"
case " $NFM " in *ffast-math*|*unsafe-math*|*finite-math-only*) echo "NFM guard fired"; exit 1;; esac
for lbl in PROD PRODC NFM BENCH; do
  case $lbl in PROD) f="$PROD";; PRODC) f="$PRODC";; NFM) f="$NFM";; BENCH) f="$BENCH";; esac
  g++ $f -I Source/src -I Source/src/Core/hpp Source/bench/bench_rfe_census.cpp \
      -o "/tmp/lad_$lbl" 2>/dev/null || { echo "$lbl BUILD_FAIL"; continue; }
  OMP_NUM_THREADS=12 RFE_FULL=1 "/tmp/lad_$lbl" "$ALOI" 400 10 2 0.1 2>&1 \
    | grep -v BuildV2 > "$OUT/faithful_${lbl}.log"
  g=$(grep -oE 'ck_gram=0x[0-9a-f]+' "$OUT/faithful_${lbl}.log" | head -1 | cut -d= -f2)
  r=$(grep -oE 'RKF7B N=72000 ck_rk=0x[0-9a-f]+' "$OUT/faithful_${lbl}.log" | head -1 | grep -oE '0x[0-9a-f]+$')
  n=$(grep -oE 'gram_nnz=[0-9]+' "$OUT/faithful_${lbl}.log" | head -1 | cut -d= -f2)
  m=$(grep -oE 'VmHWM=[0-9]+MB' "$OUT/faithful_${lbl}.log" | head -1)
  printf '%-6s gram_nnz=%-10s %-12s ck_gram=%-20s ck_rk=%s\n' "$lbl" "$n" "$m" "$g" "$r"
done
