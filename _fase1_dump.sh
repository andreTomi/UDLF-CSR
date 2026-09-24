#!/bin/bash
# _fase1_dump.sh -- Passo D (Fase 1 S2.4): rebuild do driver com o fast path,
# smoke PHASE_VALIDATE=1 e dump canonico bp_fase0 (V4_ROWSORT incondicional,
# congelado antes da Fase 1) vs bp_s23 (com fast path). Gate: md5 identico
# nos dumps (bit-identidade) + VALIDATE ok. Rodar via fix_crlf.
set -e
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR

echo "=== rebuild bp_s23 (Fase 1 fast path) ==="
bash /tmp/build_s23.sh

D0=/home/andre/dmp_fase0
D1=/home/andre/dmp_fase1
rm -rf "$D0" "$D1"; mkdir -p "$D0" "$D1"
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin

echo "=== canonical dumps: mult w1/w3, T=12, rep0 ==="
for wl in w1 w3; do
  mkdir -p "$D0/$wl" "$D1/$wl"   # o driver so abre, nao cria
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 PHASE_DUMP_DIR="$D0/$wl" \
    /home/andre/udlf_bin/bp_fase0 mult $wl 2 > "/tmp/dmp0_$wl.log" 2>&1
  taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 PHASE_DUMP_DIR="$D1/$wl" \
    /home/andre/udlf_bin/bp_s23  mult $wl 2 > "/tmp/dmp1_$wl.log" 2>&1
  echo "-- $wl --"
  (cd "$D0/$wl"  && md5sum * | sed 's/^/fase0 /')
  (cd "$D1/$wl"  && md5sum * | sed 's/^/fase1 /')
  n=0
  for f in "$D0/$wl"/*; do
    b=$(basename "$f")
    cmp -s "$f" "$D1/$wl/$b" && echo "IDENTICAL $wl/$b" || { echo "DIFF $wl/$b"; n=1; }
  done
  [ "$n" -eq 0 ] || { echo "DUMP GATE FAIL $wl"; exit 1; }
  grep -a '^REP,' "/tmp/dmp0_$wl.log" | cut -d, -f4,5 | sed 's/^/fase0 REP /'
  grep -a '^REP,' "/tmp/dmp1_$wl.log" | cut -d, -f4,5 | sed 's/^/fase1 REP /'
done

echo "=== PHASE_VALIDATE=1 smoke (real lhrr, REPS=1) ==="
taskset -c 0-11 env OMP_NUM_THREADS=12 PHASE_THREADS=12 PHASE_VALIDATE=1 \
  /home/andre/udlf_bin/bp_s23 real "$BIN" 72000 400 10 400 lhrr 1 > /tmp/val_smoke.log 2>&1
grep -a '^VALIDATE,' /tmp/val_smoke.log
nv=$(grep -ac '^VALIDATE,.*ok' /tmp/val_smoke.log)
nfail=$(grep -ac '^VALIDATE,.*FAIL' /tmp/val_smoke.log || true)
echo "validate ok=$nv fail=$nfail"
[ "$nfail" -eq 0 ] || { echo "VALIDATE GATE FAIL"; exit 1; }
echo "FASE1 DUMP+VALIDATE GATES DONE"