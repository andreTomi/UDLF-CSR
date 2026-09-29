// test_bfstree_parity.cpp
//
// Bit-exact parity gate for the BFSTree port (carrier A4, Source/src): the core
// must reproduce the LEGACY's own digest pins on the house fixture, from the
// legacy's original accumulation order -- not a re-derivation of "should be
// equal".
//
// The pins (and where they come from, all verifiable in-tree):
//   fixture N=800 L=300 k=8 seed=42 (self pinned at position 0), B=256
//   CK_W     = 0x2aaaa7b5841c8ee1   FNV-1a over the raw W float bits after F5
//   CK_FINAL = 0x7b70dfb313c2b151   FNV-1a over the final rk list bytes
//   NNZ_W    = 553578
// Provenance: recorded by the LEGACY itself (bench_bfstree.cpp mode `ref`,
// which embeds the verbatim repo-root BFSTree.cpp) -- the same values
// Source/tests/gate_bfstree.sh pins, and the same ones re-measured per regime
// in Data/phase_profile/numerics_prod/gold_ref800_*.log. The verbatim freeze
// (repo-root BFSTree.cpp/BFSTree.hpp, kept out of git by policy) carries the
// md5 chain in Data/phase_profile/bfstree_close/README.md ("Procedência do
// golden"). The fixture generator below mirrors the bench's gen_rklists()
// exactly (mt19937 + std::shuffle + self moved to index 0), so the input bytes
// are the ones those pins were produced from.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
//       -I Source/src/Core/hpp Source/tests/test_bfstree_parity.cpp \
//       -o /tmp/test_bfstree_parity
// Run: /tmp/test_bfstree_parity   (exit 0 = bit-identical to the legacy)
//
// -ffp-contract=off is the house's strict-IEEE posture for this gate (the
// strict product target builds with the same posture). Any change to the
// kernel, to the emission order or to the narrowing moves these pins.

#include "Methods/hpp/BFSTreeCore.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

// Verbatim mirror of bench_bfstree.cpp gen_rklists (the fixture the pins were
// produced from): row i = a permutation of [0,N) with i at index 0.
std::vector<int> gen_fixture(int seed, int N, int L) {
  std::vector<int> rk((size_t)N * (size_t)L, 0);
  std::mt19937 rng(seed);
  std::vector<int> perm((size_t)N);
  for (int i = 0; i < N; ++i) perm[(size_t)i] = i;
  for (int i = 0; i < N; ++i) {
    std::shuffle(perm.begin(), perm.end(), rng);
    auto it = std::find(perm.begin(), perm.end(), i);
    std::swap(perm[0], *it);
    for (int j = 0; j < L; ++j)
      rk[(size_t)i * (size_t)L + (size_t)j] = perm[(size_t)j];
  }
  return rk;
}

int failures = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("  ok  : %s\n", what);
  }
}

// Structural invariants that must hold for ANY input (cheap, and they catch the
// classes of bug the pins alone would report as "some digest changed").
void check_invariants(const std::vector<int>& rk_in, const std::vector<int>& rk_out,
                      int N, int L) {
  bool self0 = true, perm = true;
  for (int i = 0; i < N && (self0 || perm); ++i) {
    if (rk_out[(size_t)i * (size_t)L] != i) self0 = false;
    std::vector<int> a(rk_in.begin() + (size_t)i * (size_t)L,
                       rk_in.begin() + (size_t)(i + 1) * (size_t)L);
    std::vector<int> b(rk_out.begin() + (size_t)i * (size_t)L,
                       rk_out.begin() + (size_t)(i + 1) * (size_t)L);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) perm = false;   // F9 only reorders; the set must survive
  }
  expect(self0, "self at position 0 in every output row (F9 pin)");
  expect(perm, "every output row is a permutation of its input row");
}

}  // namespace

int main() {
  const int N = 800, L = 300, k = 8, seed = 42;
  const long B = 256;

  std::printf("BFSTree parity gate: core (A4) vs the legacy's recorded pins\n");
  std::printf("fixture N=%d L=%d k=%d seed=%d B=%ld (gen_rklists mirror)\n", N, L,
              k, seed, B);

  const std::vector<int> rk_in = gen_fixture(seed, N, L);
  std::vector<int> rk = rk_in;

  BFSTreeCore core(rk, (size_t)N, (size_t)L, k, B);
  core.run();

  const uint64_t ck_w = core.ck_w();
  const uint64_t ck_final = core.ck_rk();
  std::printf("  ck_w=0x%016llx ck_final=0x%016llx nnz_w=%ld\n",
              (unsigned long long)ck_w, (unsigned long long)ck_final,
              core.nnz_w());

  expect(ck_w == 0x2aaaa7b5841c8ee1ULL, "CK_W == 0x2aaaa7b5841c8ee1 (legacy)");
  expect(ck_final == 0x7b70dfb313c2b151ULL,
         "CK_FINAL == 0x7b70dfb313c2b151 (legacy pin)");
  expect(core.nnz_w() == 553578, "NNZ_W == 553578 (legacy)");
  check_invariants(rk_in, rk, N, L);

  if (failures == 0) {
    std::printf("BFSTREE PARITY: OK (bit-identical to the legacy)\n");
    return 0;
  }
  std::printf("BFSTREE PARITY: %d FAILURE(S)\n", failures);
  return 1;
}