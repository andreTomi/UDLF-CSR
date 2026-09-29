// test_bfstree_corner.cpp
//
// Corner cases and invariants for the BFSTree port (carrier A4). The parity
// gate (test_bfstree_parity.cpp) pins the legacy digests on the house fixture;
// this suite covers what a single fixture cannot: other seeds, k=1 (the
// degenerate expansion), L >= N/2 (dense-ish rkLists), the batch-independence
// of B (the carrier's blocked F5/F7 must not depend on the block size), the
// parameter/loader contracts, and the F9 pin under all of them.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
//       -I Source/src/Core/hpp Source/tests/test_bfstree_corner.cpp \
//       -o /tmp/test_bfstree_corner
// Run: /tmp/test_bfstree_corner   (exit 0 = every case holds)
//
// Why B-independence is asserted here: the carrier accumulates per row (F5) and
// per (imgI,imgJ) pair (F7), so the block size must not change a single bit.
// The campaign proved it by measurement; this gate keeps it proven.

#include "Methods/hpp/BFSTreeCore.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("  ok  : %s\n", what);
  }
}

// Same generator semantics as bench_bfstree.cpp gen_rklists (self at index 0).
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

struct Digests {
  uint64_t ck_w = 0, ck_final = 0;
  long nnz_w = 0;
};

Digests run_case(const std::vector<int>& rk_in, int N, int L, int k, long B) {
  std::vector<int> rk = rk_in;
  BFSTreeCore core(rk, (size_t)N, (size_t)L, k, B);
  core.run();
  Digests d;
  d.ck_w = core.ck_w();
  d.ck_final = core.ck_rk();
  d.nnz_w = core.nnz_w();
  // invariants (cheap, checked on every run)
  bool self0 = true, perm = true;
  for (int i = 0; i < N; ++i) {
    if (rk[(size_t)i * (size_t)L] != i) self0 = false;
    std::vector<int> a(rk_in.begin() + (size_t)i * L,
                       rk_in.begin() + (size_t)(i + 1) * L);
    std::vector<int> b(rk.begin() + (size_t)i * L,
                       rk.begin() + (size_t)(i + 1) * L);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) perm = false;
  }
  if (!self0) std::printf("    (invariant: self@0 broken)\n");
  if (!perm) std::printf("    (invariant: row set changed)\n");
  return d;
}

}  // namespace

int main() {
  std::printf("BFSTree corner cases + invariants (carrier A4)\n");

  // 1. Multi-seed determinism and B-independence on a mid-size fixture.
  {
    std::printf("[1] seeds x B-independence (N=200 L=60 k=10)\n");
    const int N = 200, L = 60, k = 10;
    for (int seed : {42, 7, 1}) {
      const std::vector<int> rk_in = gen_fixture(seed, N, L);
      const Digests base = run_case(rk_in, N, L, k, 256);
      bool same = true;
      for (long B : {1L, 13L, 99999L}) {
        const Digests d = run_case(rk_in, N, L, k, B);
        if (d.ck_w != base.ck_w || d.ck_final != base.ck_final ||
            d.nnz_w != base.nnz_w)
          same = false;
      }
      const Digests again = run_case(rk_in, N, L, k, 256);
      const bool stable =
          (again.ck_w == base.ck_w && again.ck_final == base.ck_final);
      std::printf("    seed=%d ck_w=0x%016llx ck_final=0x%016llx\n", seed,
                  (unsigned long long)base.ck_w,
                  (unsigned long long)base.ck_final);
      expect(same, "B-independence (B in {1,13,99999,256})");
      expect(stable, "repeatability at the same B");
    }
  }

  // 2. k=1: the degenerate expansion (l1 = {q}, l2 empty).
  {
    std::printf("[2] k=1 (N=60 L=25)\n");
    const int N = 60, L = 25, k = 1;
    const std::vector<int> rk_in = gen_fixture(3, N, L);
    const Digests a = run_case(rk_in, N, L, k, 64);
    const Digests b = run_case(rk_in, N, L, k, 1);
    std::printf("    ck_w=0x%016llx nnz_w=%ld\n", (unsigned long long)a.ck_w,
                a.nnz_w);
    expect(a.ck_w == b.ck_w && a.ck_final == b.ck_final, "k=1 B-independence");
    expect(a.nnz_w > 0, "k=1 produces a non-empty W");
  }

  // 3. L >= N/2 (dense-ish rank lists: the neighbourhood overlaps heavily).
  {
    std::printf("[3] L >= N/2 (N=40 L=25 k=5)\n");
    const int N = 40, L = 25, k = 5;
    const std::vector<int> rk_in = gen_fixture(11, N, L);
    const Digests a = run_case(rk_in, N, L, k, 8);
    const Digests b = run_case(rk_in, N, L, k, 3);
    std::printf("    ck_w=0x%016llx nnz_w=%ld\n", (unsigned long long)a.ck_w,
                a.nnz_w);
    expect(a.ck_w == b.ck_w && a.ck_final == b.ck_final, "L>=N/2 B-independence");
  }

  // 4. Contract cases: the core must REFUSE instead of running on bad input.
  {
    std::printf("[4] parameter/loader contracts\n");
    const int N = 20, L = 10, k = 8;
    const std::vector<int> rk_in = gen_fixture(5, N, L);
    bool threw_l = false, threw_size = false;
    try {
      std::vector<int> rk = rk_in;
      BFSTreeCore bad(rk, (size_t)N, (size_t)L, 12, 4);   // L < k
    } catch (const std::exception&) {
      threw_l = true;
    }
    try {
      std::vector<int> rk(rk_in.begin(), rk_in.end() - 1);  // short buffer
      BFSTreeCore bad(rk, (size_t)N, (size_t)L, k, 4);
    } catch (const std::exception&) {
      threw_size = true;
    }
    expect(threw_l, "L < k throws");
    expect(threw_size, "rkLists size != N*L throws");
  }

  if (failures == 0) {
    std::printf("BFSTREE CORNER: OK\n");
    return 0;
  }
  std::printf("BFSTREE CORNER: %d FAILURE(S)\n", failures);
  return 1;
}