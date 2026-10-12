#!/usr/bin/env bash
# run_regime_probe.sh -- Fase 0-B: pin the legacy oracle under THREE build
# regimes and record ck_rk (bits) + MAP, so "bit-identical to legacy" is
# unambiguous (the task's new risk: the original UDLF and this repo's strict
# gate are DIFFERENT flag sets).
#
# Regimes (flags never retyped where the Makefile is the source of truth):
#   R1 master  : -std=gnu++14 -O3                      (UDLF-master/Makefile;
#                                                       the binary users run;
#                                                       NO fast-math, NO omp)
#   R2 strict  : -std=gnu++2b -O2 -ffp-contract=off -fopenmp  (this repo's gate)
#   R3 product : <Source/Makefile CFLAGS>              (fast-math + march/native)
#
# PROD_FLAGS comes from `make -C Source -s show-flags` (prod_numerics.sh rule).
# The oracle is compiled three times, run on the SAME derived fixture, and the
# ck_rk lines are compared. Hypothesis (declared, measured here):
#   * R1 == R2 in bits  -> the strict gate faithfully represents the original;
#     the "legacy real" risk is ELIMINATED without building UDLF-master whole.
#   * R3 diverges       -> already documented in rfe_numerics_prod; here it is
#     re-confirmed end-to-end on a real derived dataset.
#
# Fixture: corel5k L400 k10 t2 pa0.1 (the Fase 0 baseline shape), derived with
# provenance by mk_rk_from_leading_id.py. N=5000 keeps each regime run seconds.
#
# Output (OUT, logs only; binaries in BINOUT stay out of the dossier):
#   regime_probe.txt   -- full transcript (builds + runs + comparison)
#   regime_ck.csv      -- regime,ck_rk,MAP@L,P@10
#
# Usage (WSL, repo root):  bash Source/tests/run_regime_probe.sh
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-"$ROOT/Data/phase_profile/rfe_fast/raw"}
BINOUT=${BINOUT:-/tmp/regime_probe}
WORK=${WORK:-/tmp/rk_real}
mkdir -p "$OUT" "$BINOUT" "$WORK" || exit 1
INC="-I Source/src -I Source/src/Core/hpp -I Source/tests"
SRC="Source/tests/regime_probe.cpp"

fail() { echo "REGIME FAILED: $*" >&2; exit 1; }
say()  { echo "[regime] $*"; }

# ------------------------------------------------------------- fixture ------
DS=corel5k; N=5000; L=400; K=10; T=2; PA=0.1
CLS="Datasets/corel5k/corel5k_classes.txt"
RK="$WORK/corel5k_L${L}.txt"
[ -f "$CLS" ] || fail "classes file missing: $CLS"
if [ ! -f "$RK" ]; then
  say "deriving $RK (provenance build)..."
  python3 Source/tests/mk_rk_from_leading_id.py \
    Datasets/corel5k/ACC.txt "$L" "$RK" "corel5k_L${L}" \
    > "$OUT/mk_rk_corel5k_L${L}.log" 2>&1 || fail "rk derivation (see log)"
fi

# ------------------------------------------------------------- flags --------
# R3 (product) is read from the Makefile, never retyped.
PROD_FLAGS=$(make -C Source -s show-flags 2>/dev/null \
             | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD_FLAGS" ] || fail "could not extract 'Optimization flags:' from Makefile"
# R1 (master) exactly as UDLF-master/Makefile: `CCOPTION = I./src std=gnu++14 O3`.
R1_FLAGS="-std=gnu++14 -O3"
# R2 (strict) exactly as the parity gates / udlf-strict minus the product extras.
R2_FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"

say "R1 master : $R1_FLAGS"
say "R2 strict : $R2_FLAGS"
say "R3 product: $PROD_FLAGS"

REPORT="$OUT/regime_probe.txt"
: > "$REPORT"
{
  echo "# Fase 0-B -- regime probe of the legacy oracle (rfe_ref_verbatim.hpp)"
  echo "# fixture: $DS N=$N L=$L k=$K t=$T pa=$PA  (self pinned at index 0)"
  echo "R1_master_flags = $R1_FLAGS"
  echo "R2_strict_flags = $R2_FLAGS"
  echo "R3_prod_flags   = $PROD_FLAGS"
  echo
} >> "$REPORT"

# ------------------------------------------------- build + run each regime ---
run_regime() {  # name flags
  local name=$1 flags=$2
  local bin="$BINOUT/probe_$name" out="$BINOUT/${name}.u32" log="$BINOUT/${name}.log"
  say "building R$name ..." >&2
  # shellcheck disable=SC2086
  $CXX $flags $INC "$SRC" -o "$bin" >> "$REPORT" 2>&1 \
    || { echo "BUILD FAILED for R$name (see $REPORT)"; return 1; }
  say "running  R$name ..." >&2
  "$bin" "$RK" "$N" "$L" "$K" "$T" "$PA" "$out" > "$log" 2>&1 \
    || { echo "RUN FAILED for R$name (see $log)"; return 1; }
  cat "$log" >> "$REPORT"
  echo >> "$REPORT"
  grep -o 'ck_rk=0x[0-9a-f]*' "$log" | head -1 | cut -d= -f2
}

ck1=$(run_regime 1 "$R1_FLAGS") || fail "R1"
ck2=$(run_regime 2 "$R2_FLAGS") || fail "R2"
ck3=$(run_regime 3 "$PROD_FLAGS") || fail "R3"

# --------------------------------------------------------- MAP per regime ---
map_of() {  # u32 -> MAP@L (percent), via the evaluator
  python3 Source/bench/rfe_quality.py metrics --rk "$1" --bin --L "$L" \
    --classes "$CLS" --ks 10 --map-ks "$L" 2>/dev/null \
    | grep -o 'MAP@[0-9]*,[0-9.]*' | head -1 | cut -d, -f2
}
p10_of() {
  python3 Source/bench/rfe_quality.py metrics --rk "$1" --bin --L "$L" \
    --classes "$CLS" --ks 10 --map-ks "$L" 2>/dev/null \
    | grep -o 'P@10,[0-9.]*' | head -1 | cut -d, -f2
}
m1=$(map_of "$BINOUT/1.u32"); p1=$(p10_of "$BINOUT/1.u32")
m2=$(map_of "$BINOUT/2.u32"); p2=$(p10_of "$BINOUT/2.u32")
m3=$(map_of "$BINOUT/3.u32"); p3=$(p10_of "$BINOUT/3.u32")

# ------------------------------------------------------------- verdict ------
CSV="$OUT/regime_ck.csv"
echo "regime,ck_rk,MAP@$L,P@10" > "$CSV"
echo "R1_master,$ck1,$m1,$p1" >> "$CSV"
echo "R2_strict,$ck2,$m2,$p2" >> "$CSV"
echo "R3_product,$ck3,$m3,$p3" >> "$CSV"

{
  echo "== regime_ck.csv =="
  cat "$CSV"
  echo
  echo "== comparison =="
  if [ "$ck1" = "$ck2" ]; then
    echo "R1(master) == R2(strict) in bits -> strict gate FAITHFULLY represents"
    echo "the original UDLF build; the 'legacy real' risk is ELIMINATED."
  else
    echo "R1(master) != R2(strict) -> strict gate does NOT reproduce the original"
    echo "build bits; the legacy oracle must be pinned to ONE regime explicitly."
  fi
  if [ "$ck1" = "$ck3" ]; then
    echo "R1(master) == R3(product) in bits (unexpected; product has fast-math)."
  else
    echo "R1(master) != R3(product) -> as documented, the product (fast-math)"
    echo "build diverges from the oracle; EXACT mode must refuse it."
  fi
  echo "REGIMEVERDICT,$([ "$ck1" = "$ck2" ] && echo R1EQ2 || echo R1NE2),$([ "$ck1" = "$ck3" ] && echo R1EQ3 || echo R1NE3)"
} | tee -a "$REPORT"

echo
say "Artifacts: $REPORT , $CSV"
