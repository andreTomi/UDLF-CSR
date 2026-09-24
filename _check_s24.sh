#!/bin/bash
# check_s24_state.sh -- diagnostico rapido do estado da campanha s24
D=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Data/phase_profile/etapa_b/s24
echo "== processos pendurados =="
ps -o pid,etime,pcpu,cmd -p 28320,46207 2>/dev/null || echo "smokes GONE"
echo
for d in raw_ab12_drift1 raw_ab12 raw_ab1_drift1 raw_ab1 raw_abcprr \
         raw_ab12_f1 raw_ab1_f1 raw_abcprr_f1; do
  echo "== $d =="
  ls "$D/$d" 2>/dev/null | wc -l
  for f in "$D/$d"/canary_*.txt; do
    [ -e "$f" ] || continue
    printf "  %s: " "$(basename "$f")"
    grep -a '^REP,' "$f" | tail -1
  done
  echo "  newest file:"
  ls -lat "$D/$d" | sed -n 2p
done
echo
echo "== agg mtimes vs raw mtimes =="
ls -la "$D/agg"
