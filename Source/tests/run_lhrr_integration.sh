#!/usr/bin/env bash
# LHRR_SPARSE integration: K-a checkpoints, serial/OpenMP framework parity,
# deterministic repeats and ASan/UBSan coverage on the shared production core.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK=${WORK:-/tmp/lhrr_integration}
mkdir -p "$WORK"

INC=(-I Source/src -I Source/src/Config/hpp -I Source/src/Config/models
  -I Source/src/Core/hpp -I Source/src/Methods/hpp
  -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp)

# This parity gate requires IEEE arithmetic: explicitly disable fast-math and
# native-architecture specialization, and keep FMA contraction disabled.
GATE_FLAGS=(-fno-fast-math -ffp-contract=off)

g++ -std=gnu++23 -O2 "${GATE_FLAGS[@]}" -fopenmp "${INC[@]}" \
  Source/tests/test_lhrr_core.cpp -o "$WORK/core"
g++ -std=gnu++23 -O2 "${GATE_FLAGS[@]}" -fopenmp "${INC[@]}" \
  Source/tests/test_lhrr_integration.cpp -o "$WORK/framework"
g++ -std=gnu++23 -O1 -g -fsanitize=address,undefined -fopenmp \
  -fno-omit-frame-pointer "${GATE_FLAGS[@]}" "${INC[@]}" \
  Source/tests/test_lhrr_core.cpp -o "$WORK/core_san"
echo "LHRR_GATE_FLAGS,-O2,${GATE_FLAGS[*]},no_-march=native"
printf 'LHRR_INPUT_SHA256,'
sha256sum UDL_sandbox/fixtures/real/corel5k/rk_L400.txt \
  UDL_sandbox/fixtures/real/brodatz/rk_L400.txt | sed 's#  #,#'
g++ -std=gnu++17 -O2 "${GATE_FLAGS[@]}" \
  -Iports/LHRR/k_b -Iports/LHRR/k_c \
  ports/LHRR/k_c/test_k_c.cpp -o "$WORK/lhrr_kc"

# Rebuild the trusted K-a runner in the same strict arithmetic regime. Existing
# Corel5k snapshots in ports/LHRR/goldens/corel5k are historical 100x20 files;
# generate fresh 5000x400 checkpoints from the official real fixture instead.
KA_WORK="$WORK/k_a"
mkdir -p "$KA_WORK"
KA_INC=(-Iudlf_original/src/Methods -IUDL_sandbox/shim)
KA_FLAGS=(-std=gnu++17 -O2 -fno-fast-math -ffp-contract=off)
g++ "${KA_FLAGS[@]}" "${KA_INC[@]}" -include UDL_sandbox/shim/Udl.hpp \
  -c ports/LHRR/goldens/src/LHRR_golden.cpp -o "$KA_WORK/LHRR_golden.o"
g++ "${KA_FLAGS[@]}" "${KA_INC[@]}" -include UDL_sandbox/shim/Udl.hpp \
  -c ports/LHRR/goldens/src/main_golden.cpp -o "$KA_WORK/main.o"
g++ "${KA_FLAGS[@]}" "${KA_INC[@]}" -include UDL_sandbox/shim/Udl.hpp \
  -c UDL_sandbox/shim/Udl.cpp -o "$KA_WORK/Udl.o"
g++ "${KA_FLAGS[@]}" -IUDL_sandbox/shim \
  -c UDL_sandbox/shim/Core/Exec.cpp -o "$KA_WORK/Exec.o"
g++ "${KA_FLAGS[@]}" "$KA_WORK/LHRR_golden.o" "$KA_WORK/main.o" \
  "$KA_WORK/Udl.o" "$KA_WORK/Exec.o" -o "$KA_WORK/lhrr_ka"

# Populate required synthetic CP8 inputs before the core/framework checks.
# The core driver checks against the serial checkpoint oracle in 1/2/4/8 threads,
# plus a self-not-at-position-zero adversarial row fixture.
for row in 'syn_small 100 20 5 2' 'syn_hub 100 20 5 2' \
           'syn_ties 100 20 5 2' 'syn_k_eq_l 100 20 20 2' \
           'syn_n47 47 10 3 1'; do
  read -r name n L k t <<< "$row"
  if [[ "$name" == syn_small ]]; then
    sed \
      -e 's|INPUT_FILE = ports/LHRR/k_c/fixtures/syn_hub.txt|INPUT_FILE = ports/LHRR/k_c/fixtures/syn_small.txt|' \
      ports/LHRR/goldens/config_syn_hub.ini > "$WORK/syn_small.ini"
    "$KA_WORK/lhrr_ka" "$WORK/syn_small.ini" syn_small "$WORK/syn_small_golden" \
      > "$WORK/syn_small_k_a.log"
    cp "$WORK/syn_small_golden/syn_small_iter2_CP8_c0-of-1.simp" \
      "$WORK/syn_small_iter2_CP8_c0-of-1.simp"
  else
    cp "ports/LHRR/goldens/$name/${name}_iter${t}_CP8_c0-of-1.simp" \
      "$WORK/${name}_iter${t}_CP8_c0-of-1.simp"
  fi
done

fixtures=(
  "syn_small 100 20 5 2"
  "syn_hub 100 20 5 2"
  "syn_ties 100 20 5 2"
  "syn_k_eq_l 100 20 20 2"
  "syn_n47 47 10 3 1"
)

echo '=== LHRR_SPARSE core versus K-a CP8; in-process deterministic repeat ==='
for row in "${fixtures[@]}"; do
  read -r name n L k t <<< "$row"
  "$WORK/core" "$n" "$L" "$k" "$t" \
    "ports/LHRR/k_c/fixtures/${name}.txt" \
    "$WORK/${name}_iter${t}_CP8_c0-of-1.simp"
done

echo '=== framework MethodFactory/Method::run integration ==='
for name in syn_ties syn_hub; do
  n=100; L=20; k=5; t=2
  for threads in 1 2 8; do
    OMP_DYNAMIC=FALSE "$WORK/framework" "$n" "$L" "$k" "$t" \
      "ports/LHRR/k_c/fixtures/${name}.txt" \
      "$WORK/${name}_iter${t}_CP8_c0-of-1.simp" \
      "$WORK/framework_${name}_t${threads}" framework "$name" "$threads"
  done
done

echo '=== ASan+UBSan LHRR core ==='
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 OMP_DYNAMIC=FALSE \
  "$WORK/core_san" 100 20 5 2 \
  ports/LHRR/k_c/fixtures/syn_ties.txt \
  "$WORK/syn_ties_iter2_CP8_c0-of-1.simp"

echo 'LHRR_INTEGRATION_SUMMARY,core=5,framework=6,sanitized_openmp=1,failures=0'

echo '=== real dataset K-a/K-c checkpoints: Corel5k + Brodatz ==='
for dataset in corel5k brodatz; do
  if [[ "$dataset" == corel5k ]]; then n=5000; else n=1776; fi
  real="$ROOT/UDL_sandbox/fixtures/real/$dataset/rk_L400.txt"
  real_work="$WORK/real_$dataset"
  mkdir -p "$real_work"
  cp "UDL_sandbox/fixtures/real/$dataset/classes.txt" "$real_work/classes.txt"
  seq 0 $((n - 1)) | sed 's/$/.jpg/' > "$real_work/images.txt"
  sed \
    -e "s/SIZE_DATASET = 100/SIZE_DATASET = $n/" \
    -e "s|INPUT_FILE = ports/LHRR/k_c/fixtures/syn_hub.txt|INPUT_FILE = $real|" \
    -e "s|INPUT_FILE_LIST = Data/fixtures/syn_lists.txt|INPUT_FILE_LIST = $real_work/images.txt|" \
    -e "s|INPUT_FILE_CLASSES = Data/fixtures/syn_lists.txt|INPUT_FILE_CLASSES = $real_work/classes.txt|" \
    -e "s|INPUT_IMAGES_PATH = Data/fixtures/|INPUT_IMAGES_PATH = $real_work/|" \
    -e 's/PARAM_LHRR_L = 20/PARAM_LHRR_L = 400/' \
    -e 's/PARAM_LHRR_K = 5/PARAM_LHRR_K = 18/' \
    ports/LHRR/goldens/config_syn_hub.ini > "$real_work/config.ini"
  "$KA_WORK/lhrr_ka" "$real_work/config.ini" "$dataset" \
    "$real_work/golden" > "$real_work/k_a.log"
  "$WORK/core" "$n" 400 18 2 \
    "$real" "$real_work/golden/${dataset}_iter2_CP8_c0-of-1.simp"
  "$WORK/lhrr_kc" "$real" "$real_work/sparse" "$n" 400 18 2 \
    > "$real_work/k_c.log"
  python3 ports/LHRR/k_c/compare_checkpoints.py \
    --golden "$real_work/golden" --sparse "$real_work/sparse" \
    --fixture "$dataset" --n "$n" --l 400 --k 18 --t 2 \
    --checkpoints C0,C1,C2,C8 --n_ulp 4 \
    > "$real_work/compare.log"
  tail -n 18 "$real_work/compare.log"
done

echo '=== real dataset quality metrics ==='
for dataset in corel5k brodatz; do
  if [[ "$dataset" == corel5k ]]; then n=5000; else n=1776; fi
  python3 - "$dataset" "$n" "$WORK" <<'PY'
import pathlib, struct, sys
dataset, n, work = sys.argv[1], int(sys.argv[2]), pathlib.Path(sys.argv[3])
snap = work / f"real_{dataset}/golden/{dataset}_iter2_CP8_c0-of-1.simp"
blob = snap.read_bytes()
count = struct.unpack_from('<Q', blob, 48)[0]
if count != n * 400:
    raise SystemExit(f"bad ranking size in {snap}: {count}")
ids = struct.unpack_from('<' + 'I' * count, blob, 60)
out = work / f"real_{dataset}/final.txt"
with out.open('w') as f:
    for row in range(n):
        f.write(' '.join(map(str, ids[row * 400:(row + 1) * 400])) + '\n')
PY
  python3 Source/bench/rfe_quality.py metrics \
    --rk "$WORK/real_$dataset/final.txt" --L 400 \
    --classes "UDL_sandbox/fixtures/real/$dataset/classes.txt" \
    --ks 5,10,20,50 --map-ks 5,10,20,50,400 \
    > "$WORK/real_$dataset/quality.log"
  grep -E 'METRIC,(MAP@400|P@10|Recall@10)|SELFCHK,MAP@400' \
    "$WORK/real_$dataset/quality.log"
done

echo '=== Corel5k/Brodatz framework MethodFactory/Method::run integration ==='
g++ -std=gnu++23 -O2 "${GATE_FLAGS[@]}" -fopenmp "${INC[@]}" \
  Source/tests/test_lhrr_integration.cpp -o "$WORK/framework_real"
for dataset in corel5k brodatz; do
  if [[ "$dataset" == corel5k ]]; then n=5000; else n=1776; fi
  for threads in 1 2; do
    OMP_DYNAMIC=FALSE "$WORK/framework_real" "$n" 400 18 2 \
      "$ROOT/UDL_sandbox/fixtures/real/$dataset/rk_L400.txt" \
      "$WORK/real_$dataset/golden/${dataset}_iter2_CP8_c0-of-1.simp" \
      "$WORK/framework_real_${dataset}_t${threads}" framework "$dataset" "$threads"
  done
done

echo '=== production Method output quality (Corel5k, one thread) ==='
python3 Source/bench/rfe_quality.py metrics \
  --rk "$WORK/framework_real_corel5k_t1/output/reranked_lists.txt" --L 400 \
  --classes UDL_sandbox/fixtures/real/corel5k/classes.txt \
  --ks 5,10,20,50 --map-ks 5,10,20,50,400 \
  > "$WORK/framework_real_corel5k_t1/quality.log"
grep -E 'METRIC,(MAP@400|P@10|Recall@10)|SELFCHK,MAP@400' \
  "$WORK/framework_real_corel5k_t1/quality.log"

echo 'LHRR_REAL_GATE_SUMMARY,corel5k_k-a-k-c=C0/C1/C2/C8,framework=4,production_quality=1,failures=0'