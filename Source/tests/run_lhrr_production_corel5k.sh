#!/usr/bin/env bash
# Exercise the actual Source/src/Core/cpp/Main.cpp entrypoint for a real LHRR
# configuration without linking unrelated unavailable backends from Method.cpp.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK=${WORK:-/tmp/lhrr_integration}
mkdir -p "$WORK/output"

INC=(-I Source/src -I Source/src/Config/hpp -I Source/src/Config/models
  -I Source/src/Core/hpp -I Source/src/Methods/hpp
  -I Source/src/Evaluation/hpp -I Source/src/Utils/hpp)
FLAGS=(-std=gnu++23 -O2 -fno-fast-math -ffp-contract=off -fopenmp)

# Main.cpp is the production CLI. This registration TU wires just the method
# under test and keeps absent MKL/BFSTree registrations out of this test link.
cat > "$WORK/lhrr_sparse_register.cpp" <<'CPP'
#include "Methods/hpp/LHRRSparse.hpp"
MethodRegister<LHRRSparse> LHRRSparse::signInMethod("LHRR_SPARSE");
CPP
g++ "${FLAGS[@]}" "${INC[@]}" -c Source/src/Core/cpp/Main.cpp \
  -o "$WORK/Main.o"
g++ "${FLAGS[@]}" "${INC[@]}" -c "$WORK/lhrr_sparse_register.cpp" \
  -o "$WORK/register.o"
g++ "${FLAGS[@]}" "${INC[@]}" "$WORK/Main.o" "$WORK/register.o" \
  -o "$WORK/udlf_lhrr_strict" -lpthread

n=5000
real="$ROOT/UDL_sandbox/fixtures/real/corel5k/rk_L400.txt"
sed -e "s/SIZE_DATASET = 100/SIZE_DATASET = $n/" \
    -e "s|INPUT_FILE = ports/LHRR/k_c/fixtures/syn_hub.txt|INPUT_FILE = $real|" \
    -e "s|INPUT_FILE_LIST = Data/fixtures/syn_lists.txt|INPUT_FILE_LIST = $WORK/images.txt|" \
    -e "s|INPUT_FILE_CLASSES = Data/fixtures/syn_lists.txt|INPUT_FILE_CLASSES = $ROOT/UDL_sandbox/fixtures/real/corel5k/classes.txt|" \
    -e "s|INPUT_IMAGES_PATH = Data/fixtures/|INPUT_IMAGES_PATH = $WORK/|" \
    -e 's/UDL_METHOD = LHRR$/UDL_METHOD = LHRR_SPARSE/' \
    -e 's/PARAM_LHRR_L = 20/PARAM_LHRR_SPARSE_L = 400/' \
    -e 's/PARAM_LHRR_K = 5/PARAM_LHRR_SPARSE_K = 18/' \
    -e 's/PARAM_LHRR_T = 2/PARAM_LHRR_SPARSE_T = 2/' \
    -e '/PARAM_LHRR_SPARSE_THREADS/d' \
    -e 's|OUTPUT_FILE_PATH = output/results.txt|OUTPUT_FILE_PATH = output/corel5k_matrix.txt|' \
    -e 's|OUTPUT_LOG_FILE_PATH = output/log.txt|OUTPUT_LOG_FILE_PATH = output/corel5k.log|' \
    "$ROOT/ports/LHRR/goldens/config_syn_hub.ini" > "$WORK/config.ini"
seq 0 $((n - 1)) | sed 's/$/.jpg/' > "$WORK/images.txt"
cat "$ROOT/UDL_sandbox/fixtures/real/corel5k/classes.txt" > "$WORK/classes.txt"

# The same Corel5k K-a/K-c exact checkpoints are already built and validated by
# run_lhrr_integration.sh. Reuse that fresh K-a final ranking when available;
# keep this Main.cpp gate focused on the production adapter/entrypoint output.
if [[ ! -f "$WORK/real_corel5k/golden/corel5k_iter2_CP8_c0-of-1.simp" ]]; then
  echo "missing fresh K-a oracle; run test-lhrr before test-lhrr-production" >&2
  exit 1
fi

(cd "$WORK" && OMP_DYNAMIC=FALSE ./udlf_lhrr_strict "$WORK/config.ini" \
  > "$WORK/framework_main.log" 2>&1)
grep -F 'Running Method: LHRR_SPARSE' "$WORK/framework_main.log"
grep -F 'Execution Completed!' "$WORK/framework_main.log"
python3 - "$WORK/output/reranked_lists.txt" \
  "$WORK/real_corel5k/golden/corel5k_iter2_CP8_c0-of-1.simp" <<'PY'
import pathlib, struct, sys
actual = [int(token) for token in pathlib.Path(sys.argv[1]).read_text().split()]
blob = pathlib.Path(sys.argv[2]).read_bytes()
if blob[:4] != b'SIMP' or struct.unpack_from('<I', blob, 4)[0] != 1:
    raise SystemExit('invalid K-a CP8 snapshot')
n, l = struct.unpack_from('<QQ', blob, 24)
count = struct.unpack_from('<Q', blob, 48)[0]
if (n, l, count) != (5000, 400, 2_000_000):
    raise SystemExit(f'unexpected K-a CP8 shape: n={n}, L={l}, count={count}')
expected = list(struct.unpack_from('<' + 'I' * count, blob, 60))
if actual != expected:
    for index, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise SystemExit(f'production ranking differs at row={index // l}, col={index % l}: {got} != {want}')
    raise SystemExit(f'production ranking length differs: {len(actual)} != {count}')
print('LHRR_PRODUCTION_ORACLE,PASS,ids=2000000,exact=1')
PY
python3 "$ROOT/Source/bench/rfe_quality.py" metrics \
  --rk "$WORK/output/reranked_lists.txt" --L 400 \
  --classes "$ROOT/UDL_sandbox/fixtures/real/corel5k/classes.txt" \
  --ks 5,10,20,50 --map-ks 5,10,20,50,400 > "$WORK/quality.log"
grep -E 'METRIC,(MAP@400|P@10|Recall@10)|SELFCHK,MAP@400' "$WORK/quality.log"
printf 'LHRR_PRODUCTION_ENTRYPOINT,PASS,n=5000,L=400,k=18,t=2,threads=default1\n'