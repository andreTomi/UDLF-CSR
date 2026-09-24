#!/bin/bash
# wrapper: roda os 3 modos da Fase 0 sequencialmente
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
python3 Source/bench/fix_crlf.py _p3_diag.sh > /dev/null
for m in ab12 ab1 abcprr; do
  bash /tmp/_p3_diag.sh "$m"
done
echo ALL_MODES_DONE
