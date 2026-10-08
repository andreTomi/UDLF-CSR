#!/usr/bin/env bash
# step3d_ksweep.sh -- k sweep, WRITTEN TO THE REPOSITORY, and resumable.
#
# Rewritten after the previous campaign was lost: the sweep wrote its raws to
# /tmp, the WSL instance restarted, and the table in FINDINGS became
# unreproducible from committed artefacts -- which the bench README forbids
# ("se a tabela do dossie nao e regeneravel a partir dos brutos commitados, ela
# nao existe"). Two rules now, both learned the hard way:
#
#   1. LOGS GO IN THE REPO. The derived rank lists stay in a work dir (they are
#      ~100 MB and regenerable from Datasets/ by a deterministic converter);
#      the logs are the durable artefact and they are small.
#   2. RESUMABLE. A point whose log already carries a verdict is skipped, so a
#      restart costs nothing. A liveness check is never `pgrep -f <pattern>`:
#      that matches its own command line and reports a dead sweep as running.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
BIN=${BIN:-/tmp/rfe_sweep}
OUT=${OUT:-Data/phase_profile/rfe_ksweep}
WORK=${WORK:-/tmp/rk_real}
TMO_CHEAP=${TMO_CHEAP:-2400}
TMO_DENSE=${TMO_DENSE:-14400}
mkdir -p "$OUT" "$WORK" || exit 1

# The derived datasets are byte-reproducible; verify rather than trust.
regen() {
  local src=$1 L=$2 out=$3 md5=$4
  if [ -f "$out" ] && [ "$(md5sum < "$out" | cut -d' ' -f1)" = "$md5" ]; then
    echo "  have $out (md5 ok)"; return 0
  fi
  python3 Source/tests/mk_rk_from_leading_id.py "$src" "$L" "$out" "$(basename "$out")" \
    | grep -e wrote -e REFUSED
  local got; got=$(md5sum < "$out" | cut -d' ' -f1)
  [ "$got" = "$md5" ] || { echo "  MD5 MISMATCH $out: got $got want $md5"; return 1; }
  echo "  built $out md5=$md5"
}

echo "=== datasets (deterministic, md5-verified) ==="
regen Datasets/corel5k/ACC.txt 400  "$WORK/corel5k_L400.txt"  abc57bdd7aed6ee66e7fbfc32cbd3301
regen Datasets/corel5k/ACC.txt 1000 "$WORK/corel5k_L1000.txt" 0fba3ef9d5fab266dceda912c7082877
regen Datasets/linnaeus/linnaeus5_resnet50.txt 400 "$WORK/linnaeus_L400.txt" dde830919922f758424282efaec41613

SUM="$OUT/summary.tsv"
[ -f "$SUM" ] || printf 'dataset\tN\tL\tk\trc\tgram_nnz\tzeros_pct\tck_gram\tck_rk\teetie\teetie_of\n' > "$SUM"

run() {
  local ds=$1 N=$2 L=$3 k=$4
  local f="$WORK/${ds}_L${L}.txt"
  local log="$OUT/${ds}_L${L}_k${k}.log"
  if grep -q 'RFE PARITY' "$log" 2>/dev/null; then
    echo "skip  $ds L=$L k=$k (already has a verdict)"; return 0
  fi
  [ -f "$f" ] || { echo "MISSING $f"; return 1; }
  local tmo=$TMO_CHEAP; [ "$k" -ge 50 ] && tmo=$TMO_DENSE
  local t0 t1; t0=$(date +%s)
  OMP_NUM_THREADS=12 timeout "$tmo" "$BIN" "$N" "$L" "$k" 2 42 "$f" > "$log" 2>&1
  local rc=$?
  t1=$(date +%s)
  local g r n z v tie tieof
  g=$(grep -o 'ck_gram=0x[0-9a-f]*' "$log" | head -1 | cut -d= -f2)
  r=$(grep -o 'core ck_rk=0x[0-9a-f]*' "$log" | tail -1 | cut -d= -f2)
  n=$(grep -o 'core gram nnz: [0-9]*' "$log" | head -1 | awk '{print $4}')
  z=$(grep -o '([0-9.]*% of' "$log" | head -1 | tr -d '(')
  v=$(grep -c 'bit-identical to the legacy' "$log")
  tie=$(grep -o 'exactly equal normalised values: [0-9]*' "$log" | awk '{print $5}')
  tieof=$(grep -o 'exactly equal normalised values: [0-9]* of [0-9]*' "$log" | awk '{print $7}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$ds" "$N" "$L" "$k" "$rc" "${n:-}" "${z:-}" "${g:-}" "${r:-}" "${v:-0}" \
    "${tie:-}" "${tieof:-}" >> "$SUM"
  printf 'done  %-9s L=%-5s k=%-3s rc=%-4s %4ss  exact=%s  gram=%s  eetie=%s\n' \
    "$ds" "$L" "$k" "$rc" "$((t1-t0))" "$v" "${g:-none}" "${tie:-?}"
}

echo "=== cheap points ==="
run corel5k 5000 400 5
run corel5k 5000 400 10
run corel5k 5000 400 20
run corel5k 5000 1000 5
run corel5k 5000 1000 20
run linnaeus 8000 400 5
run linnaeus 8000 400 20
echo "=== dense points (long timeout) ==="
run corel5k 5000 400 50
run linnaeus 8000 400 50
run corel5k 5000 1000 50
echo "=== summary ==="
column -t -s $'\t' "$SUM"
