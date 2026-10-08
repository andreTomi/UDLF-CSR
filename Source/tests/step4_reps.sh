#!/bin/bash
# Build + run + summarise in ONE invocation. State does not persist between tool
# calls here (/tmp does not survive), so a campaign split across invocations
# silently measures nothing -- which it did once already.
set -u
cd /mnt/c/Users/andre/Documents/phd/side_projects/UDLF-CSR/UDLF-CSR || exit 1
D=Data/phase_profile/rfe_numerics_prod
mkdir -p "$D"
ALOI=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp \
    Source/bench/bench_rfe_census.cpp -o /tmp/reps_bin 2>/dev/null || { echo BUILD_FAIL; exit 1; }
echo "build ok; no other RFE process running:"
ps -eo comm= | grep -E '^(rfe_sweep|cr|cr1|reps_bin|census_run)$' | head -3
echo "(none listed above = clean)"
for T in 12 1; do
  for i in 1 2 3 4; do
    OMP_NUM_THREADS=$T OMP_WAIT_POLICY=PASSIVE RFE_FULL=1 RFE_F1PROF=1 \
      /tmp/reps_bin "$ALOI" 400 10 2 0.1 2>&1 | grep -v BuildV2 > "$D/rep_t${T}_${i}.log"
  done
  echo "T=$T done"
done
echo
for T in 12 1; do
  echo "===== T=$T  (median of runs 2-4; run 1 discarded) ====="
  for ph in "f1_rank_norm" "f7a rerank" "f6 Gram" "f7b CPRR"; do
    vals=$(for i in 2 3 4; do
              grep -E "^ *$ph" "$D/rep_t${T}_${i}.log" | head -1 \
              | sed -E 's/.*[ ]([0-9]+\.[0-9]+) s.*/\1/'
            done | sort -n)
    med=$(echo "$vals" | sed -n 2p); lo=$(echo "$vals" | sed -n 1p); hi=$(echo "$vals" | sed -n 3p)
    sp=$(echo "$lo $hi" | awk '{printf "%.1f", ($2>0)?100*($2-$1)/$2:0}')
    printf '  %-13s median=%8.3f  min=%8.3f  max=%8.3f  spread=%s%%\n' "$ph" "$med" "$lo" "$hi" "$sp"
  done
  tot=$(for i in 2 3 4; do grep -oE 'total=[0-9.]+s' "$D/rep_t${T}_${i}.log" | head -1 | cut -d= -f2 | tr -d 's'; done | sort -n)
  echo "  total        median=$(echo "$tot" | sed -n 2p)  runs: $(echo $tot)"
  echo "  VmHWM        $(for i in 2 3 4; do grep -oE 'VmHWM=[0-9]+MB' "$D/rep_t${T}_${i}.log" | head -1; done | tr '\n' ' ')"
  echo "  ck_rk        $(for i in 2 3 4; do grep -oE 'RKF7B N=72000 ck_rk=0x[0-9a-f]+' "$D/rep_t${T}_${i}.log" | head -1 | grep -oE '0x[0-9a-f]+$'; done | sort -u | tr '\n' ' ')"
done
