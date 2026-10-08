#!/usr/bin/env bash
set -u
cd /mnt/c/Users/andre/Documents/phd/side_projects/UDLF-CSR/UDLF-CSR || exit 1
RAW=Data/phase_profile/rfe_numerics_prod/reps.txt.raw
for T in 12 1; do
  echo "===== T=$T  (median of runs 2-4; run 1 discarded) ====="
  for ph in "f1_rank_norm" "f7a rerank" "f6 Gram" "f7b CPRR"; do
    vals=$(for i in 2 3 4; do
              grep -E "^ *$ph" "$RAW.t$T.$i" | head -1 | sed -E 's/.*[ ]([0-9]+\.[0-9]+) s.*/\1/'
            done | sort -n)
    med=$(echo "$vals" | sed -n 2p); lo=$(echo "$vals" | sed -n 1p); hi=$(echo "$vals" | sed -n 3p)
    sp=$(echo "$med $hi" | awk '{print ($2>0)?100*($2-$2/1)/$2:0}')
    sp=$(echo "$lo $hi" | awk '{printf "%.1f", ($2>0)?100*($2-$1)/$2:0}')
    printf '  %-13s median=%8.3f  min=%8.3f  max=%8.3f  spread=%s%%\n' "$ph" "$med" "$lo" "$hi" "$sp"
  done
  tot=$(for i in 2 3 4; do grep -oE 'total=[0-9.]+s' "$RAW.t$T.$i" | head -1 | cut -d= -f2 | tr -d 's'; done | sort -n)
  echo "  total        median=$(echo "$tot" | sed -n 2p)  runs: $(echo $tot)"
  echo "  VmHWM        $(for i in 2 3 4; do grep -oE 'VmHWM=[0-9]+MB' "$RAW.t$T.$i" | head -1; done | tr '\n' ' ')"
  echo "  ck_rk        $(for i in 2 3 4; do grep -oE 'RKF7B N=72000 ck_rk=0x[0-9a-f]+' "$RAW.t$T.$i" | head -1 | grep -oE '0x[0-9a-f]+$'; done | sort -u | tr '\n' ' ')"
done
