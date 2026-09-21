#!/bin/bash
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
OUTDIR=Data/phase_profile/etapa_b/fase3
echo "=== F2_PRUNE comparison (w1, w2, w3 @ 1 thread) ==="
for w in w1 w2 w3; do
  for v in baseline fase1 fase13; do
    f=$OUTDIR/${v}_${w}_t1.txt
    if [ -f "$f" ]; then
      rep=$(grep '^REP,' "$f" | head -1 | cut -d, -f3)
      f2=$(grep 'F2_PRUNE,' "$f" | head -1 | cut -d, -f3)
      f1=$(grep 'F1_ACCUM,' "$f" | head -1 | cut -d, -f3)
      mp=$(grep 'MP_CALL,' "$f" | head -1 | cut -d, -f3)
      echo "$w,$v,f1=$f1,f2=$f2,mp=$mp,rep=$rep"
    fi
  done
done


