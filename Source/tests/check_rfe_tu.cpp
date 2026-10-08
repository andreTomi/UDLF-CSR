// check_rfe_tu.cpp -- translation-unit check for RFECore.hpp.
//
// Same purpose as Source/tests/check_bfstree_tu.cpp: the core is a header that
// only compiles when included the way the product includes it, so this file
// instantiates the class and touches every phase so that a signature change or a
// missing include fails HERE rather than in a phase that happens to use it.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp (one line)
//       -I Source/src -I Source/src/Core/hpp
//       Source/tests/check_rfe_tu.cpp -o /tmp/check_rfe_tu && /tmp/check_rfe_tu

#include "Methods/hpp/RFECore.hpp"

#include <cstdio>
#include <vector>

int main() {
  const int N = 32, L = 8, k = 4;
  std::vector<int> rk((std::size_t)N * (std::size_t)L, 0);
  for (int q = 0; q < N; ++q) {
    rk[(std::size_t)q * (std::size_t)L] = q;                 // self at 0
    for (int i = 1; i < L; ++i)
      rk[(std::size_t)q * (std::size_t)L + (std::size_t)i] = (q + i) % N;
  }
  RFECore core(rk, (std::size_t)N, (std::size_t)L, k, 1, 0.1);
  core.f1_rank_norm();
  core.f2_create_emb();
  core.f3_create_reverse_emb();
  core.f4_combine_reverse_emb();
  core.f5_sort_emb();
  core.f5_effect_estimation_emb();
  core.f6_gram_and_inc_elems();
  core.f7b_rerank_cprr_tail();
  std::printf("TU ok: sim nnz=%zu emb nnz=%zu zeros=%zu remb nnz=%zu ck_rk=0x%016llx ck_emb=0x%016llx\n",
              core.nnz_sim(), core.nnz_emb(), core.emb_exact_zeros(), core.nnz_remb(),
              (unsigned long long)core.ck_rk(), (unsigned long long)core.ck_emb());
  // F7 surfaces: the Gram, the diagonal pin, and the union prune. The prune is
  // the H8 design, so it belongs in the TU surface where a signature change fails
  // here rather than in whichever gate happens to call it.
  std::printf("  F7: gram nnz=%zu zeros=%zu  union_prune_row(0,L)=%zu ids\n",
              core.nnz_gram(), core.gram_exact_zeros(),
              core.union_prune_row(0, (std::size_t)L).size());
  // F5 digests exercise the tag_sort path, whose tie-break is part of the output
  // (it becomes F6's loop order), so it belongs in the TU surface too.
  std::printf("  F5: ee[0]=%.9g gr=%.9g sorted_ee[0]=%d ck_ee=0x%016llx"
              " ck_sorted_emb=0x%016llx ck_sorted_ee=0x%016llx\n",
              (double)core.ee()[0], (double)core.gr_value(), core.sorted_ee()[0],
              (unsigned long long)core.ck_ee(),
              (unsigned long long)core.ck_sorted_emb(),
              (unsigned long long)core.ck_sorted_ee());
  if (core.sorted_ee().size() != (std::size_t)N) {
    std::printf("TU FAIL: sorted_ee size != N\n");
    return 1;
  }
  // F4 must leave the support UNCHANGED: the legacy keeps every key and zeroes
  // the ones whose reverse partner is missing. If nnz collapses here, the kernel
  // emitted through a zero filter and F3's exact-zero guarantee is gone.
  if (core.nnz_emb() == 0) {
    std::printf("TU FAIL: emb support was emptied by F4\n");
    return 1;
  }
  // Exercise the accessors the parity gate depends on, so a change to their
  // signatures breaks this file and not just the gate.
  std::printf("  rankKScore(1)=%.9g rankKScore(k)=%.9g  (rankKScore(k) must be 0)\n",
              (double)core.rank_kscore_pub(1), (double)core.rank_kscore_pub(k));
  const CSRMatrix<float>& e = core.emb();
  std::printf("  emb rows=%u cols=%u  sim rows=%u\n", (unsigned)e.rows(),
              (unsigned)e.cols(), (unsigned)core.sim().rows());
  return (core.rank_kscore_pub(k) == 0.0f) ? 0 : 1;
}