#!/usr/bin/env bash
# CPRR_SPARSE core integration: eight ranking oracles, ASan / UBSan, deterministic
# repeats, strict / production regimes, and real-dataset quality gates. The
# framework path is tested by the following Make target in a dependency-light TU.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK=${WORK:-/tmp/cprr_integration}
mkdir -p "$WORK"

INC=(-I Source/src -I Source/src/Config/hpp -I Source/src/Config/models
  -I Source/src/Core/hpp -I Source/src/Methods/hpp
  -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp
  -I UDL_sandbox/snapshot/include -I UDL_sandbox/compare/include)

g++ -std=gnu++23 -O2 -ffp-contract=off "${INC[@]}" \
  Source/tests/test_cprr_core.cpp -o "$WORK/core_strict"
g++ -std=gnu++23 -O2 -ffast-math "${INC[@]}" \
  Source/tests/test_cprr_core.cpp -o "$WORK/core_fast_math"
g++ -std=gnu++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffp-contract=off "${INC[@]}" Source/tests/test_cprr_core.cpp \
  -o "$WORK/core_san"

# fixture, n, L, k, t, ranking text, original oracle valid
fixtures=(
  "syn_rand_small 64 16 10 2 UDL_sandbox/fixtures/synthetic/syn_rand_small.txt no"
  "syn_k_eq_l 48 8 8 2 UDL_sandbox/fixtures/synthetic/syn_k_eq_l.txt no"
  "syn_hub 64 16 10 2 UDL_sandbox/fixtures/synthetic/syn_hub.txt no"
  "syn_ties 32 16 10 2 UDL_sandbox/fixtures/synthetic/syn_ties.txt no"
  "syn_n47 47 10 5 2 UDL_sandbox/fixtures/synthetic/syn_n47.txt yes"
  "syn_no_self0 64 16 10 2 UDL_sandbox/fixtures/synthetic/syn_no_self0.txt yes"
  "real_corel5k_L400 5000 400 20 2 UDL_sandbox/fixtures/real/corel5k/rk_L400.txt yes"
  "real_brodatz_L400 1776 400 20 2 UDL_sandbox/fixtures/real/brodatz/rk_L400.txt yes"
)

expected_for() {
  local name=$1 original_ok=$2
  if [[ "$original_ok" == yes ]]; then
    printf 'ports/CPRR/goldens/%s/Cs_2_lists.snp' "$name"
  else
    printf 'ports/CPRR/out_sparse/%s/Cs_2_lists.snp' "$name"
  fi
}

echo '=== strict regime: K1-K6 against D-0005/D-0006 ==='
for row in "${fixtures[@]}"; do
  read -r name n L k t input original_ok <<< "$row"
  expected=$(expected_for "$name" "$original_ok")
  "$WORK/core_strict" "$n" "$L" "$k" "$t" "$input" "$expected" "$name" "$WORK/${name}.strict.txt"
  "$WORK/core_strict" "$n" "$L" "$k" "$t" "$input" "$expected" "$name" "$WORK/${name}.rep1.txt" >/dev/null
  "$WORK/core_strict" "$n" "$L" "$k" "$t" "$input" "$expected" "$name" "$WORK/${name}.rep2.txt" >/dev/null
  cmp "$WORK/${name}.rep1.txt" "$WORK/${name}.rep2.txt"
done

echo '=== production -ffast-math regime: K1-K6 against the same oracle ==='
for row in "${fixtures[@]}"; do
  read -r name n L k t input original_ok <<< "$row"
  expected=$(expected_for "$name" "$original_ok")
  "$WORK/core_fast_math" "$n" "$L" "$k" "$t" "$input" "$expected" "$name" "$WORK/${name}.fast.txt"
done

echo '=== repeatability: strict output byte-identical ==='
for row in "${fixtures[@]}"; do
  read -r name n L k t input original_ok <<< "$row"
  cmp "$WORK/${name}.rep1.txt" "$WORK/${name}.rep2.txt"
  cmp "$WORK/${name}.strict.txt" "$WORK/${name}.fast.txt"
done

echo '=== ASan+UBSan: all eight fixtures ==='
for row in "${fixtures[@]}"; do
  read -r name n L k t input original_ok <<< "$row"
  expected=$(expected_for "$name" "$original_ok")
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
    "$WORK/core_san" "$n" "$L" "$k" "$t" "$input" "$expected" "$name" "$WORK/${name}.san.txt"
  cmp "$WORK/${name}.strict.txt" "$WORK/${name}.san.txt"
done

echo '=== real-dataset quality gates: Corel5k + Brodatz ==='
python3 Source/bench/rfe_quality.py metrics --rk "$WORK/real_corel5k_L400.strict.txt" \
  --classes UDL_sandbox/fixtures/real/corel5k/classes.txt --ks 5,10,20,50 \
  --map-ks 5,10,20,50,400 > "$WORK/corel5k_quality.log"
python3 Source/bench/rfe_quality.py metrics --rk "$WORK/real_brodatz_L400.strict.txt" \
  --classes UDL_sandbox/fixtures/real/brodatz/classes.txt --ks 5,10,20,50 \
  --map-ks 5,10,20,50,400 > "$WORK/brodatz_quality.log"
grep -q 'SELFCHK,MAP@400.*PASS' "$WORK/corel5k_quality.log"
grep -q 'SELFCHK,MAP@400.*PASS' "$WORK/brodatz_quality.log"
grep -E 'METRIC,(MAP@400|P@10|Recall@10)|SELFCHK,MAP@400' \
  "$WORK/corel5k_quality.log" "$WORK/brodatz_quality.log"

echo 'CPRR_INTEGRATION_SUMMARY,strict=8,fast_math=8,sanitized=8,repeatable=8,failures=0'