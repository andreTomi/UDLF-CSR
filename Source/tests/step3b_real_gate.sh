#!/usr/bin/env bash
# step3b_real_gate.sh -- Step 3 / Layer 0: ABSOLUTE regime pins (X8).
#
# Three jobs, in order of increasing cost:
#
#   [1] the pin can PASS  -- BENCH regime, pinned synthetic fixture
#   [2] the pin can FAIL  -- PROD regime, same fixture, SAME source, and it MUST
#                            fail. A pin that cannot fail is X6 (vacuous success)
#                            in a different costume, so [2] is not optional.
#   [3] the pin HOLDS on real data -- three derived datasets, regenerated here
#                            from Datasets/ and md5-verified, never read from /tmp.
#
# PROD_FLAGS comes from the Makefile and is never retyped (prod_numerics.sh rule).
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
# OUT receives LOGS ONLY (point it at the repo for durable evidence); binaries
# go to BINOUT, which must stay OUT of the dossier -- a gate that drops parity_*
# into the evidence directory dirties the repository for no reason.
OUT=${OUT:-/tmp/step3b}
BINOUT=${BINOUT:-/tmp/step3b_bin}
WORK=${WORK:-/tmp/rk_real}
INC="-I Source/src -I Source/src/Core/hpp -I Source/tests"
BINCH_FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"
mkdir -p "$OUT" "$BINOUT" "$WORK" || exit 1

fail() { echo "STEP3B FAILED: $*" >&2; exit 1; }

PROD_FLAGS=$(make -C Source -s show-flags 2>/dev/null \
             | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD_FLAGS" ] || fail "nao consegui extrair 'Optimization flags:' do Makefile"
PROD_FLAGS="$PROD_FLAGS -ffp-contract=off -ffast-math -funsafe-math-optimizations -ffinite-math-only"
echo "prod_flags : $PROD_FLAGS"
echo "bench_flags: $BINCH_FLAGS"
echo "cpu        : $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"

# ------------------------------------------------------------------ [1] [2] --
echo
echo "[1/2] pin PASSES under the strict regime (BENCH)"
$CXX $BINCH_FLAGS $INC Source/tests/test_rfe_parity.cpp -o "$BINOUT/parity_bench" 2>/dev/null \
  || fail "build parity_bench"
"$BINOUT/parity_bench" > "$OUT/bench_pinned.log" 2>&1
rc=$?
grep -v BuildV2 "$OUT/bench_pinned.log" | grep -E 'X8|RFE PARITY' | sed 's/^/    /'
[ "$rc" = 0 ] || fail "pin NAO passou sob o regime estrito -- o pin esta errado"

echo
echo "[2/2] pin FAILS under the product's regime (PROD)  <-- the point of X8"
$CXX $PROD_FLAGS $INC Source/tests/test_rfe_parity.cpp -o "$BINOUT/parity_prod" 2>/dev/null \
  || fail "build parity_prod"
"$BINOUT/parity_prod" > "$OUT/prod_pinned.log" 2>&1
rc=$?
grep -v BuildV2 "$OUT/prod_pinned.log" | grep -E 'X8|RFE PARITY|FAIL' | sed 's/^/    /'
if [ "$rc" = 0 ]; then
  fail "PROD PASSOU -- o pin nao distingue regime. Gate de vacuo (X6)."
fi
echo "    -> pin falhou sob PROD (rc=$rc), como deve."

# -------------------------------------------------------------------- [3] --
echo
echo "[3/3] pins HOLD on real data -- 6 configs (3 datasets; k in {5,10,20})"
python3 Source/tests/mk_rk_from_leading_id.py Datasets/corel5k/ACC.txt 400 \
        "$WORK/corel5k_L400.txt"  corel5k_L400  | grep -e wrote -e REFUSED | sed "s/^/    /" \
  || fail "construir corel5k_L400"
python3 Source/tests/mk_rk_from_leading_id.py Datasets/corel5k/ACC.txt 1000 \
        "$WORK/corel5k_L1000.txt" corel5k_L1000 | grep -e wrote -e REFUSED | sed "s/^/    /" \
  || fail "construir corel5k_L1000"
python3 Source/tests/mk_rk_from_leading_id.py Datasets/linnaeus/linnaeus5_resnet50.txt 400 \
        "$WORK/linnaeus_L400.txt"  linnaeus_L400  | grep -e wrote -e REFUSED | sed "s/^/    /" \
  || fail "construir linnaeus_L400"

# N L K EXPECTED_CK_GRAM EXPECTED_CK_RK
# ck_gram values are the FAITHFUL post-run() digests (the sweep's ck_gram column
# is the F6-section, pre-t-loop, superseded). ck_rk came from the sweep's E2E,
# which is faithful for every k (FINDINGS, k sweep correction).
CASES="
5000 400 5   0x0ff2642751009152 0x36f430f2ddc9d424
5000 400 10  0x28bdf35f87bfff3d 0xb2d84793a18b7e43
5000 400 20  0x3218b119e9aabc2c 0x2c264a0c6b975da9
5000 1000 10 0x4ecba5630543276a 0x7ce087aad8d272f1
5000 1000 20 0x045f449c61d55fb5 0x58162fc8a58e0739
8000 400 10  0xa584ebbe38e86571 0x09b624c5caae064c
"
# herestring, NOT `echo | while`: a pipe runs the loop in a SUBshell, so a
# `fail` inside it would exit the subshell and the script would report success.
# That is X6 -- a gate that prints the divergence and then carries on -- and it
# is precisely the failure mode this gate exists to detect.
rc_all=0
while read -r N L k g r; do
  [ -z "${N:-}" ] && continue
  case "$N" in 5000) base=corel5k ;; *) base=linnaeus ;; esac
  f="$WORK/${base}_L${L}.txt"
  log="$OUT/real_${N}_${L}_k${k}.log"
  [ -f "$f" ] || { echo "    N=$N L=$L k=$k -> arquivo ausente $f"; rc_all=$((rc_all+1)); continue; }

  # RESUME BY CONTENT. The key is the FINAL VERDICT line -- the binary's last
  # act -- never an intermediate one: X9 prints BEFORE the pins are asserted, so
  # a process killed in between leaves an "X9 control:" line with no verdict and
  # would be skipped as done. Precedent: rfe_ksweep/corel5k_L400_k50.log,
  # truncated mid-F5 without a verdict, recorded as NOT MEASURED.
  if grep -q '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: ' "$log" 2>/dev/null; then
    echo "    N=$N L=$L k=$k -> veredito ja presente em $(basename "$log") (retomada por conteudo)"
  else
    OMP_NUM_THREADS=12 "$BINOUT/parity_bench" "$N" "$L" "$k" 2 42 "$f" > "$log" 2>&1
  fi

  cmp_n=0
  # Presence is an ASSERTION with its own message (X6): a line that never ran
  # is a different defect from a line that ran and failed, and the two must
  # never share a message -- "ausencia nao e deriva".
  grep -q 'REAL FAITHFUL cfg=' "$log" 2>/dev/null \
    || { echo "      -> AUSENCIA: 'REAL FAITHFUL' (pin nao avaliado neste config)"; cmp_n=$((cmp_n+1)); }
  grep -q 'E2E Gram (faithful):' "$log" 2>/dev/null \
    || { echo "      -> AUSENCIA: 'E2E Gram (faithful):' (paridade do Gram nao avaliada)"; cmp_n=$((cmp_n+1)); }
  grep -q 'X9 control:' "$log" 2>/dev/null \
    || { echo "      -> AUSENCIA: 'X9 control:' (controle negativo nao rodou)"; cmp_n=$((cmp_n+1)); }

  verdict=$(grep -o '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: .*' "$log" 2>/dev/null | head -1)
  if [ -z "$verdict" ]; then
    echo "      -> SEM VEREDITO (processo interrompido antes do fim?)"
    cmp_n=$((cmp_n+1))
  elif printf '%s' "$verdict" | grep -q ': OK '; then
    echo "      veredito: OK"
  else
    echo "      -> VEREDITO FAIL: $verdict"
    cmp_n=$((cmp_n+1))
  fi

  got_g=$(grep -o 'REAL FAITHFUL cfg=[^ ]* ck_gram=0x[0-9a-f]*' "$log" 2>/dev/null | grep -o '0x[0-9a-f]*$')
  got_r=$(grep -o 'E2E final rkLists: legacy ck_rk=0x[0-9a-f]*' "$log" 2>/dev/null | grep -o '0x[0-9a-f]*$')
  got_x9=$(grep -o 'short (no f7a, embRR x1) ck_rk=0x[0-9a-f]*' "$log" 2>/dev/null | grep -o '0x[0-9a-f]*$')
  got_g_section=$(grep -o 'REAL pin(cfg=F6 section).*ck_gram=0x[0-9a-f]*' "$log" 2>/dev/null | grep -o '0x[0-9a-f]*$')
  echo "      (F6 section, pre-t-loop, for contrast): $got_g_section"
  echo "    N=$N L=$L k=$k"
  echo "      ck_gram got=$got_g want=$g"
  echo "      ck_rk    got=$got_r want=$r"
  echo "      X9 curto got=${got_x9:-<ausente>}"
  [ "$got_g" != "$g" ] && { echo "      -> ck_gram DIVERGIU"; cmp_n=$((cmp_n+1)); }
  [ "$got_r" != "$r" ] && { echo "      -> ck_rk DIVERGIU"; cmp_n=$((cmp_n+1)); }
  # Redundante com o expect interno de proposito: se o log tiver X9 mas o
  # curto == fiel, o shell tambem marca -- duas independentes, uma mensagem.
  if [ -n "$got_x9" ] && [ "$got_x9" = "$got_r" ]; then
    echo "      -> X9: curto == fiel: CONTROLE VAZIO NESTE CONFIG (propriedade do dado, nao bug do core)"
    cmp_n=$((cmp_n+1))
  fi
  rc_all=$((rc_all + cmp_n))
done <<< "$CASES"

echo
if [ "$rc_all" != 0 ]; then
  fail "$rc_all divergencia(s) nos pins de dado real (ver $OUT/real_*.log)"
fi
echo "-- agregador final: veredito de TODOS os configs de dado real --"
agg_fail=0
agg_n=0
while read -r N L k g r; do
  [ -z "${N:-}" ] && continue
  agg_n=$((agg_n + 1))
  log="$OUT/real_${N}_${L}_k${k}.log"
  if [ ! -f "$log" ]; then
    echo "  SEM LOG     : $(basename "$log")"
    agg_fail=$((agg_fail + 1))
    continue
  fi
  v=$(grep -o '^RFE PARITY F1+F2+F3+F4+F5+F6+F7: .*' "$log" 2>/dev/null | head -1)
  if [ -z "$v" ]; then
    echo "  SEM VEREDITO: $(basename "$log")"
    agg_fail=$((agg_fail + 1))
  elif printf '%s' "$v" | grep -q ': OK '; then
    echo "  OK          : $(basename "$log")"
  else
    echo "  FAIL        : $(basename "$log") -> $v"
    agg_fail=$((agg_fail + 1))
  fi
done <<< "$CASES"
[ "$agg_n" = 6 ] || { echo "agregador: $agg_n configs contados (esperado 6)"; agg_fail=$((agg_fail + 1)); }
[ "$agg_fail" = 0 ] || fail "$agg_fail log(s) fora de OK no agregador final (FAIL ou sem veredito)"

echo "STEP3B: OK -- pin passa em BENCH, FALHA em PROD (prova de que distingue), e"
echo "        os 6 pins de dado real conferem; E2E Gram fiel e X9 presentes e"
echo "        distinguindo em todos."
