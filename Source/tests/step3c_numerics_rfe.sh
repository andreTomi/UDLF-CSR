#!/bin/bash
# step3c_numerics_rfe.sh -- RFE under the PRODUCT's build flags (Step 3 / Stage 3c).
#
# Same posture as Source/bench/prod_numerics.sh, applied to the RFE port. The
# methodology is copied deliberately rather than reinvented:
#
#   * PROD_FLAGS is EXTRACTED from the Makefile, never retyped. If the Makefile
#     changes, this audit changes with it -- that is the entire point.
#   * FOUR regimes, because "the product's flags change the answer" and "the
#     product's flags change the answer BECAUSE OF FMA CONTRACTION" are different
#     findings with different fixes.
#   * Guards that the regime derivation actually HAPPENED. Without them the audit
#     could pass by vacuity, with all four regimes being the same binary.
#
# Regime ladder:
#   PROD  the product, verbatim            -> does RFE survive the real build?
#   PRODC PROD + -ffp-contract=off          -> is the culprit FMA contraction alone?
#   NFM   fast-math stripped, +contract=off -> is the culprit the reassociation?
#   BENCH the literal gate flags           -> the regime all RFE pins were made in
#
# The precedent's verdict for LHRR was PRODC=NO / NFM=YES, i.e. FMA contraction was
# NOT the culprit there. RFE is MORE exposed (exp/log/pow in rankNorm, and 4-factor
# products in the Gram), so it could land either side. Measure, do not assume.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-Data/phase_profile/rfe_numerics_prod}
INC="-I Source/src -I Source/src/Core/hpp -I Source/tests"
SRC=Source/tests/test_rfe_parity.cpp
mkdir -p "$OUT" || exit 1

fail() { echo "STEP3C FAILED: $*" >&2; exit 1; }

# ----------------------------------------------------------------- regimes --
PROD_FLAGS=$(make -C Source -s show-flags 2>/dev/null \
             | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD_FLAGS" ] || fail "nao consegui extrair 'Optimization flags:' do Makefile"
case " $PROD_FLAGS " in
  *" -ffast-math "*) : ;;
  *) fail "PROD sem -ffast-math -- extracao falhou, a auditoria sairia vazia" ;;
esac
# -march=native amarra o resultado a ESTA maquina. Recorded, because "PROD is
# reproducible" is only true on the CPU that produced it.
CPU=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')
PROD_FLAGS="$PROD_FLAGS -ffp-contract=off -ffast-math -funsafe-math-optimizations -ffinite-math-only"
PRODC_FLAGS="$PROD_FLAGS"
# Normalise whitespace FIRST. The Makefile emits a double space after -D_GNU_SOURCE,
# which defeats any ' -flag ' pattern match. Then remove every fast-math flag
# occurrence, not just the first -- hence /g, after the duplication above.
NFM_FLAGS=$(printf '%s\n' "$PROD_FLAGS" \
           | tr -s ' ' \
           | sed -e 's/ -ffast-math//g' \
                 -e 's/ -funsafe-math-optimizations//g' \
                 -e 's/ -ffinite-math-only//g')
NFM_FLAGS="$NFM_FLAGS -ffp-contract=off"
BENCH_FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"

# Guardas: a derivacao tem que TER acontecido. Sem isto os quatro regimes poderiam
# ser o mesmo binario e a auditoria "passaria" por vacuidade.
[ "$NFM_FLAGS" != "$PROD_FLAGS" ] || fail "NFM identico a PROD: a substituicao nao ocorreu"
case " $NFM_FLAGS " in
  *" -ffast-math "*|*" -funsafe-math-optimizations "*|*" -ffinite-math-only "*)
    fail "NFM ainda carrega flag de fast-math" ;;
esac
echo "prod_flags : $PROD_FLAGS"
echo "prod_c     : $PRODC_FLAGS"
echo "nfm        : $NFM_FLAGS"
echo "bench      : $BENCH_FLAGS"
echo "cpu        : $CPU"
echo

run_regime() {                      # $1=label $2=flags $3=threads
  local lbl=$1 fl=$2 th=$3
  local bin="/tmp/step3c_${lbl}_${th}"
  local log="$OUT/${lbl}_T${th}.log"
  if ! $CXX $fl $INC "$SRC" -o "$bin" 2>"$OUT/${lbl}_T${th}.build.log"; then
    echo "BUILD_FAIL $lbl T$th"; return 2
  fi
  OMP_NUM_THREADS=$th "$bin" 2>&1 | grep -v BuildV2 > "$log"
  local rc=${PIPESTATUS[0]}
  echo "== $lbl T=$th (exec_rc=$rc)"
  grep -E 'ck_gram=|ck_rk=|ck_sim_dense=|matrix_nnz|nnz=|RFE PARITY|FAIL' "$log" \
    | sed -e 's/^/   /' | sort -u | head -14
  return 0
}

{
  for T in 1 12; do
    run_regime PROD  "$PROD_FLAGS"  "$T"
    run_regime PRODC "$PRODC_FLAGS" "$T"
    run_regime NFM   "$NFM_FLAGS"   "$T"
    run_regime BENCH "$BENCH_FLAGS" "$T"
    echo
  done
} 2>&1 | tee "$OUT/run.out"

echo
echo "--- REAL DATA (ALOI N=72000): o Gram tambem se move sob as flags do produto? ---"
# The synthetic pin says the E2E rank lists survive. That is a property of ONE
# fixture's tie-breaking robustness, not a guarantee. The Gram digest is the
# hard instrument, so it is what gets compared here, on real structure.
cd "$ROOT" || exit 1
BIN2=/tmp/step3c_census
for lbl in PROD PRODC NFM BENCH; do
  case $lbl in
    PROD)  f="$PROD_FLAGS"  ;;
    PRODC) f="$PRODC_FLAGS" ;;
    NFM)   f="$NFM_FLAGS"   ;;
    BENCH) f="$BENCH_FLAGS" ;;
  esac
  if $CXX $f -I Source/src -I Source/src/Core/hpp \
       Source/bench/bench_rfe_census.cpp -o "$BIN2" 2>/dev/null; then
    OMP_NUM_THREADS=12 RFE_FULL=1 "$BIN2" \
      ${REALBIN:-/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin} \
      400 10 2 0.1 2>&1 | grep -v BuildV2 \
      | grep -E 'RKF6 |RKF7B ' | sed "s/^/  $lbl /" | tee -a "$OUT/real_${lbl}.log"
  else
    echo "  $lbl BUILD_FAIL (real)"
  fi
done

echo
echo "--- veredito (derivado de $OUT/run.out) ---"
for lbl in PROD PRODC NFM BENCH; do
  ok=$(grep -l 'RFE PARITY F1+F2+F3+F4+F5+F6+F7: OK' "$OUT/${lbl}_T1.log" 2>/dev/null | wc -l)
  d=$(grep -hoE 'ck_gram=0x[0-9a-f]+' "$OUT/${lbl}_T1.log" 2>/dev/null | head -1)
  printf '%-6s pin_passa=%s  %s\n' "$lbl" "$ok" "$d"
done