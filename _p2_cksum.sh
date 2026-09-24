#!/bin/bash
# _p2_cksum.sh -- (a) REP checksum equality between A/B arms, every mode.
# REP line: REP,<i>,<total_ms>,<nnz>,<checksum> -- field 5 is the (weak)
# column-ordered double sum over the result matrix.  pre==new here is a
# necessary condition; the strong proof is the PHASE_RK_OUT md5 gate in
# _p2_rkcmp.sh.
set -u
D=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Data/phase_profile/etapa_b/s23p2
rc=0
for m in ab12 ab1 abcprr; do
  echo "== $m =="
  all=""
  for f in "$D"/raw_"$m"/*.txt; do
    case "$f" in *warm*) continue ;; esac
    ck=$(grep -a '^REP,' "$f" | cut -d, -f5 | sort -u | tr '\n' ' ')
    printf '%-10s %s\n' "$(basename "$f" .txt)" "$ck"
    all="$all $ck"
  done
  n=$(echo $all | tr ' ' '\n' | sed '/^$/d' | sort -u | wc -l)
  if [ "$n" -eq 1 ]; then
    echo "GATE $m: PASS (1 distinct checksum across arms x reps)"
  else
    echo "GATE $m: FAIL ($n distinct checksums)"
    rc=1
  fi
  echo
done
exit $rc
