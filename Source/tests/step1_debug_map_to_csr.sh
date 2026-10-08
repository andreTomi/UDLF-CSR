#!/usr/bin/env bash
# step1_debug_map_to_csr.sh -- isolate the segfault inside map_to_csr().
#
# H5 crashes with SIGSEGV *after* build_from_partial_results prints its
# "[BuildV2] Total triplets: 5461" line but *before* map_to_csr returns. The
# same function is called from H4 (5461 keys -> combine, 8496 -> remb) and those
# complete. So the crash is not in the engine's build -- it is in what map_to_csr
# does around it.
#
# The suspicious line is the emission ORDER inside map_to_csr: it reads
# `rows.find((int)q)` and pushes keys VERBATIM, but the engine's builder DEDUPES
# duplicates and requires a `ranges` vector sized to the allocator count. If a
# row emits a column >= m, the builder writes out of bounds in the V3 scatter.
# emb keys are image ids in [0,N), and m == N here, so that should hold -- but
# this probe prints the actual min/max key and m so the claim is checked, not
# assumed.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/probe_map_to_csr}
FLAGS=${FLAGS:-"-std=gnu++2b -O0 -g -ffp-contract=off -fopenmp"}
cat > /tmp/probe_map.cpp <<'CPP'
#include "Core/hpp/SparseMatrix.hpp"
#include <cstdio>
#include <map>
#include <vector>
using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;
static M map_to_csr(const std::map<int, std::map<int, float>>& rows, int n, int m) {
  M out((idx_t)n, (idx_t)m, 0);
  int nt = omp_get_max_threads();
  std::vector<M::PaddedAllocator> alloc;
  alloc.reserve((size_t)nt);
  for (int i = 0; i < nt; ++i) alloc.emplace_back();
  std::vector<std::pair<idx_t, idx_t>> ranges((size_t)nt);
  std::size_t chunk = ((std::size_t)n + (std::size_t)nt - 1) / (std::size_t)nt;
  #pragma omp parallel
  {
    int tid = omp_get_thread_num();
    auto& a = *alloc[(size_t)tid].ptr;
    std::size_t r0 = std::min((std::size_t)n, (std::size_t)tid * chunk);
    std::size_t r1 = std::min((std::size_t)n, (std::size_t)(tid + 1) * chunk);
    ranges[(size_t)tid] = {(idx_t)r0, (idx_t)r1};
    for (std::size_t q = r0; q < r1; ++q) {
      const auto it = rows.find((int)q);
      if (it == rows.end()) continue;
      for (const auto& kv : it->second) {
        Triplet* t = a.allocate();
        t->row = (idx_t)q; t->col = (idx_t)kv.first; t->value = kv.second;
        t->is_set = false;
      }
    }
  }
  M::build_from_partial_results(out, alloc, ranges);
  return out;
}
int main() {
  const int N = 120, m = 120;
  std::map<int, std::map<int, float>> rows;
  int mn = 1 << 30, mx = -1; long tot = 0;
  for (int q = 0; q < N; ++q) {
    for (int j = 0; j < 45; ++j) {           // 45 keys/row -> 5400, like emb
      const int c = (q * 7 + j * 13) % N;
      rows[q][(float)c] = 1.0f;
      if (c < mn) mn = c;
      if (c > mx) mx = c;
      ++tot;
    }
  }
  std::printf("keys=%ld min=%d max=%d m=%d threads=%d\n", tot, mn, mx, m,
              omp_get_max_threads());
  std::fflush(stdout);
  M r = map_to_csr(rows, N, m);
  std::printf("survived: nnz=%zu rows=%u cols=%u\n", r.nnz(),
              (unsigned)r.rows(), (unsigned)r.cols());
  // SECOND build of the SAME input. The real test crashes on its second
  // map_to_csr call (H4 builds emb/emb_post/remb then H5 builds emb_post again),
  // while the first four builds succeed -- so this call is the discriminator.
  std::printf("second build starting\n");
  std::fflush(stdout);
  M r2 = map_to_csr(rows, N, m);
  std::printf("second build survived: nnz=%zu\n", r2.nnz());
  return 0;
}
CPP
$CXX $FLAGS -I Source/src -I Source/src/Core/hpp /tmp/probe_map.cpp -o "$BIN" || exit 1
OMP_NUM_THREADS=${THREADS:-1} "$BIN"
echo "PROBE EXIT=$?"