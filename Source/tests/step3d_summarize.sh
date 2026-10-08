#!/usr/bin/env bash
# Rebuild summary.tsv from the logs. The first campaign wrote rows with "?" in the
# ee-tie columns because the grep was case-sensitive ("EXACTLY"). Regenerating
# from the logs is the point of keeping them in the repo.
cd /mnt/c/Users/andre/Documents/phd/side_projects/UDLF-CSR/UDLF-CSR || exit 1
OUT=Data/phase_profile/rfe_ksweep
SUM="$OUT/summary.tsv"
printf 'dataset\tN\tL\tk\trc\tgram_nnz\tzeros_pct\tck_gram\tck_rk\texact\teetie\teetie_of\n' > "$SUM.tmp"
for f in "$OUT"/*.log; do
  b=$(basename "$f" .log)
  ds=${b%%_*}; rest=${b#*_}
  L=${rest#L}; L=${L%%_*}; k=${rest##*_k}
  case "$ds" in corel5k) N=5000;; linnaeus) N=8000;; *) continue;; esac
  rc=$(grep -c 'RFE PARITY' "$f")
  g=$(grep -o 'ck_gram=0x[0-9a-f]*' "$f" | head -1 | cut -d= -f2)
  r=$(grep -o 'core ck_rk=0x[0-9a-f]*' "$f" | tail -1 | cut -d= -f2)
  n=$(grep -o 'core gram nnz: [0-9]*' "$f" | head -1 | awk '{print $4}')
  z=$(grep -o '([0-9.]*% of' "$f" | head -1 | tr -d '(')
  ex=$(grep -c 'bit-identical to the legacy' "$f")
  tie=$(grep -o 'equal normalised values: [0-9]*' "$f" | awk '{print $4}')
  tieof=$(grep -o 'equal normalised values: [0-9]* of [0-9]*' "$f" | awk '{print $6}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$ds" "$N" "$L" "$k" "$rc" "${n:-}" "${z:-}" "${g:-}" "${r:-}" "$ex" "${tie:-}" "${tieof:-}" >> "$SUM.tmp"
done
mv "$SUM.tmp" "$SUM"
cat "$SUM"
