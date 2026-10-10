/**
 * @file RFECore.hpp
 * @brief RFE (Reciprocal re-ranking with embeddings) on the CSRMatrix engine --
 *        Step 2 carrier. Built ONE PHASE AT A TIME, each validated against the
 *        Step 0 golden (Source/tests/test_rfe_golden.cpp) before the next is
 *        added. Never two changes measured together.
 *
 * PROVENANCE. Every phase below is the repo-root RFE.cpp with the framework glue
 * removed. The verbatim legacy is mirrored in Source/tests/rfe_ref_verbatim.hpp
 * and its recorded digests are the pins this core is measured against:
 *   fixture N=200 L=40 k=10 t=2 pa=0.1 seed=42
 *     rankNorm          ck_rk=0x3465e556d414ca79 ck_matrix=0x8afb15dd0502c7cb
 *   (see Source/tests/step0_check.sh, gate [3/3]).
 *
 * WHAT STEP 1 MEASURED (Source/tests/test_rfe_hyp.cpp, all 9 hypotheses):
 *   H1  emb == S + S*S bit-exactly, but only in the pos1 (rank position) order;
 *       the "all S then all S*S" split order diverges (64 cells measured), so a
 *       column-ordered SpGEMM does NOT reproduce the legacy here.
 *   H2  emb carries keys whose value is EXACTLY 0.0f (20.7% measured). These are
 *       load-bearing, and multiply_and_prune's |v| > 1e-9 emission filter
 *       (SparseMatrix.hpp:559) would DISCARD them -> emb cannot be built with it.
 *   H3  remb == emb^T in values, but 3035 keys have no counterpart in emb, so a
 *       bare transpose() does not reproduce remb's row set.
 *   H4  combineReverseEmb keeps every key and zeroes the asymmetric ones;
 *       CSRMatrix::combine and merge() both LOSE 3443 of 5461 keys -> a dedicated
 *       kernel is mandatory.
 *   H5  simEmb is a two-pointer merge at 0 ULP.
 *   H6  ee is invariant to the sorted_emb tie-break (3693 swaps, 0 ULP).
 *   H7  the Gram matches bit-exactly in sorted_ee order; the legacy materialises
 *       72-99% of the n^2 cells.
 *   H8  26.56% of incElems cells have Gram value exactly 0.0f, so a VALUE-ONLY
 *       top-L prune is NOT exact; the prune must select over the union of
 *       (nonzero Gram) and (zero-valued incElems).
 *   H9  a position index reproduces getPosition exactly, including l+2 absent.
 *
 * BUILD CONTRACT: as in BFSTree.hpp, bit-exactness holds only under the STRICT
 * build (no -ffast-math, no FMA contraction). RFE calls exp/log/pow in rankNorm
 * and accumulates 4-factor products in the Gram, so it is at least as exposed as
 * BFSTree. Whether the strict target is mandatory is measured in Step 4; until
 * then the core is developed and gated under -O2 -ffp-contract=off.
 *
 * COVERAGE: F1 (rankNorm) only at this revision. Phases are added one per
 * validated step, each with its own parity digest.
 */
#ifndef RFE_CORE_HPP
#define RFE_CORE_HPP

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

class RFECore {
 public:
  using M = CSRMatrix<float>;
  using idx_t = M::size_type;
  using Triplet = M::Triplet;

  // rk: row-major rank lists, n*l ints (legacy stride l). Read AND written in
  // place, exactly as the legacy's sorts do.
  RFECore(std::vector<int>& rk, size_t n, size_t l, int k, int t, double pa,
          bool performCCs = false, double th_cc = 0.0)
      : rk_(rk), n_(n), l_((int)l), k_(k), t_(t), pa_(pa), th_cc_(th_cc) {
    if (l_ > (int)n_) throw std::runtime_error("RFE: L can't be greater than N");
    // Not checked by the legacy's checkParameters (RFE.cpp:50-56), which only
    // tests l > n -- but its own createEmb reads rkLists[l*q + pos1] for
    // pos1 < k (RFE.cpp:589), so k > l reads out of bounds, and rankKScore
    // divides by log(k) (RFE.cpp:645), so k < 2 is 0/0. Fail fast instead.
    if (k_ > l_) throw std::runtime_error("RFE: need K <= L");
    if (k_ < 2) throw std::runtime_error("RFE: need K >= 2");
    if (rk_.size() != (size_t)n_ * (size_t)l_) {
      throw std::runtime_error("RFE: rkLists size != N*L");
    }
    // H27 (Etapa provisória / Section 1): the flag is STORED, not acted on here.
    // The CC stages exist as callable seams (F8a-F8f) but run() does NOT yet
    // wire the full F8g/F8h + integration sequence, so honouring the flag would
    // emit different rank lists than the legacy. run() refuses it loudly (see
    // the throw at its top); the carrier's "supported" claim is likewise false
    // until Etapa 9. Removal is the Etapa 9 acceptance criterion (grep == 0).
    // th_cc_ is already initialised in the member-init list (: th_cc_(th_cc)).
    performCCs_ = performCCs;
  }

// ------------------------------------------------------------------ F1 --
  // rankNorm (RFE.cpp:180-193) + execSortRankedLists (RFE.cpp:717-767).
  //
  // Writes EXACTLY l cells per row (assignment, not +=), so the result is
  // N*L nonzeros -- natively sparse, no N^2 anywhere. Step 1 confirmed the
  // structure; this phase reproduces the VALUES and the resulting order.
  void f1_rank_norm() {
    // F1 cost attribution (Step 3 / Step 3c). COARSE clocks only: one pair per
    // PHASE, never per cell. A clock read per cell would cost ~57.6M reads on
    // the ALOI shape (2 per cell x 28.8M cells) and would dominate the very
    // thing being measured. The split INSIDE the cell loop (position_of vs
    // rank_norm_cell) is obtained DIFFERENTIALLY instead -- by building the
    // lookup-table variant and comparing, which needs no in-loop clock at all.
    const bool prof = f1prof_.enabled;
    const double t_f1_0 = prof ? omp_get_wtime() : 0.0;
    f1prof_.threads = omp_get_max_threads();
    const double t_idx_0 = prof ? omp_get_wtime() : 0.0;
    build_position_index();
    const double t_idx_1 = prof ? omp_get_wtime() : 0.0;
    const int nt = omp_get_max_threads();
    // M:: qualification is required: PaddedAllocator is a NESTED type of
    // CSRMatrix and build_from_partial_results is a STATIC member. This is the
    // engine's documented emission path (SparseMatrix.hpp contract: emission
    // order is free, any thread, any row).
    std::vector<M::PaddedAllocator> alloc;
    alloc.reserve((std::size_t)nt);
    for (int i = 0; i < nt; ++i) alloc.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges((std::size_t)nt);
    const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

    const double t_loop_0 = prof ? omp_get_wtime() : 0.0;
    std::vector<double> p_cells((std::size_t)nt, 0.0);
    std::vector<long long> p_absent((std::size_t)nt, 0);

    // Refactored to use parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    // Each iteration processes one chunk of rows. The tid is implicit via omp_get_thread_num().
    std::vector<double> p_cells_chunk((std::size_t)nt, 0.0);
    std::vector<long long> p_absent_chunk((std::size_t)nt, 0);
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      auto& a = *alloc[(std::size_t)tid].ptr;
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      ranges[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};
      double t_row = prof ? omp_get_wtime() : 0.0;
      for (std::size_t q = r0; q < r1; ++q) {
        for (int i = 0; i < l_; ++i) {
          const int img = rk_[(std::size_t)l_ * q + (std::size_t)i];
          const int posqi = i + 1;
          const int posiq = position_of((idx_t)img, (idx_t)q);
          if (prof && posiq == l_ + 2) ++p_absent_chunk[(std::size_t)tid];
          const float v = rank_norm_cell(posqi, posiq);
          Triplet* t = a.allocate();
          t->row = (idx_t)q;
          t->col = (idx_t)img;
          t->value = v;
          t->is_set = false;
        }
        if (prof) {
          const double now = omp_get_wtime();
          p_cells_chunk[(std::size_t)tid] += now - t_row;
          t_row = now;
        }
      }
    }
    if (prof) {
      for (int i = 0; i < nt; ++i) {
        p_cells[(std::size_t)i] += p_cells_chunk[(std::size_t)i];
        p_absent[(std::size_t)i] += p_absent_chunk[(std::size_t)i];
      }
    }
    const double t_loop_1 = prof ? omp_get_wtime() : 0.0;
    M sim((idx_t)n_, (idx_t)n_, 0);
    const double t_build_0 = prof ? omp_get_wtime() : 0.0;
    M::build_from_partial_results(sim, alloc, ranges);
    sim_ = std::move(sim);
    const double t_build_1 = prof ? omp_get_wtime() : 0.0;
    if (prof) {
      f1prof_.t_index = t_idx_1 - t_idx_0;
      f1prof_.t_loop = t_loop_1 - t_loop_0;
      f1prof_.t_build = t_build_1 - t_build_0;
      f1prof_.cells = 0;
      f1prof_.absent = 0;
      for (int i = 0; i < nt; ++i) {
        f1prof_.t_cells_max = std::max(f1prof_.t_cells_max, p_cells[(std::size_t)i]);
        f1prof_.absent += p_absent[(std::size_t)i];
      }
      f1prof_.cells = (long long)n_ * (long long)l_;
    }

    // The legacy then sorts each rank list by the similarity of its members and
    // pins self to position 0. Reproduced verbatim below in sort_ranked_lists().
    const double t_sort_0 = prof ? omp_get_wtime() : 0.0;
    sort_ranked_lists();
    if (prof) {
      f1prof_.t_sortrows = omp_get_wtime() - t_sort_0;
      f1prof_.t_total = omp_get_wtime() - t_f1_0;
    }
  }

  // ------------------------------------------------------------------ F2 --
  // createEmb (RFE.cpp:583-600). This is the phase the port's shape is decided
  // by, because three measured facts constrain it (Step 1, Source/tests/test_rfe_hyp.cpp):
  //
  //  H1  emb == S + S*S bit-exactly, but ONLY when accumulated in the legacy's
  //      pos1 (RANK POSITION) order with term 1 of pos1 emitted BEFORE the k
  //      terms 2 of that same pos1. The "all of S, then all of S*S" split order
  //      diverges (64 cells measured on the pinned fixture), so a column-ordered
  //      SpGEMM does NOT reproduce the legacy. Hence: not multiply_and_prune.
  //
  //  H2  emb carries keys whose value is EXACTLY 0.0f -- 20.7% of them measured.
  //      rankKScore(K) == 1 - log(K)/log(K) == 0.0f exactly, and incEmb INSERTS
  //      the key on first sight, so those zeros are structural: they reach
  //      sorted_emb and incElems, and therefore the output rank lists.
  //      multiply_and_prune's |v| > 1e-9 emission filter (SparseMatrix.hpp:559)
  //      would DISCARD them, so this phase cannot go through it either.
  //
  //  H1b Support is at most k + k^2 columns per row, independent of n. That bound
  //      is what lets the accumulator below be O(k^2) instead of O(n).
  //
  // WHY A PER-ROW ACCUMULATOR INSTEAD OF EMITTING DUPLICATES. Emitting one record
  // per (pos1,pos2) term and letting build_from_partial_results coalesce would
  // be shorter, but the builder sorts each row by column with an UNSTABLE
  // std::sort before summing duplicates (SparseMatrix.hpp:322-336), so the order
  // in which a cell's terms are added is whatever that sort leaves behind --
  // deterministic for a given libstdc++, but NOT the legacy's (pos1,pos2) order,
  // and therefore not guaranteed bit-equal. Accumulating here fixes the order
  // explicitly: `add` is called in exactly the legacy's call sequence, so the
  // per-cell partial sums are the legacy's, term for term. One record per column
  // is then emitted, ascending, so the builder's V4 fast path sees a strictly
  // increasing row and has nothing to coalesce -- removing the dependence on
  // std::sort's tie behaviour entirely.
  void f2_create_emb() {
    // rankKScore is the legacy's expression, NOT a re-derivation:
    //   1 - log(pos)/log(k), evaluated in double, narrowed once on return.
    // Re-deriving it as (log(k)-log(pos))/log(k) matches in value here but is
    // not guaranteed to match bit-for-bit, and at pos == k the exact 0.0f
    // (which H2 shows is load-bearing) depends on log(k)/log(k) == 1.0 exactly.
    auto rank_kscore = [this](int pos) -> float {
      return (float)(1 - (log((double)pos) / log((double)k_)));
    };

    const int nt = omp_get_max_threads();
    // Per-thread accumulators, each sized to the k + k^2 bound. Independent of n,
    // so the phase's memory does not grow with the dataset.
    std::vector<EmbAcc> accs((std::size_t)nt);
    for (int i = 0; i < nt; ++i) accs[(std::size_t)i].init((std::size_t)k_ + (std::size_t)k_ * (std::size_t)k_);

    std::vector<M::PaddedAllocator> alloc;
    alloc.reserve((std::size_t)nt);
    for (int i = 0; i < nt; ++i) alloc.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges((std::size_t)nt);
    const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

    // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      auto& a = *alloc[(std::size_t)tid].ptr;
      EmbAcc& acc = accs[(std::size_t)tid];
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      ranges[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};

      for (std::size_t q = r0; q < r1; ++q) {
        acc.begin();
        // The legacy's loop, term for term. pos1 is a RANK POSITION, not a
        // column: img_n = rk[l*q + pos1], and the inner loop walks THAT image's
        // own list. Order is load-bearing (see H1 above).
        for (int pos1 = 0; pos1 < k_; ++pos1) {
          const int img_n = rk_[(std::size_t)l_ * q + (std::size_t)pos1];
          const float wn = rank_kscore(pos1 + 1);
          acc.add((idx_t)img_n, wn);                       // term 1
          for (int pos2 = 0; pos2 < k_; ++pos2) {
            const float wnn = rank_kscore(pos2 + 1);
            const int img_nn = rk_[(std::size_t)l_ * img_n + (std::size_t)pos2];
            acc.add((idx_t)img_nn, wn * wnn);               // term 2, float product
          }
        }
        // Emit one record per DISTINCT column, ascending -- including the
        // exactly-zero ones, which H2 shows must survive. No |v| filter here.
        acc.sort_columns();
        for (std::size_t s = 0; s < acc.size(); ++s) {
          Triplet* t = a.allocate();
          t->row = (idx_t)q;
          t->col = acc.col(s);
          t->value = acc.val(s);
          t->is_set = false;
        }
      }
    }
    M emb((idx_t)n_, (idx_t)n_, 0);
    M::build_from_partial_results(emb, alloc, ranges);
    emb_ = std::move(emb);
  }

  // ---- F3: createReverseEmb ----------------------------------------------------
//
// The legacy (RFE.cpp:618-632) is:
//
//     remb.clear();
//     for (q = 0..n) remb[q] = {};                 // pre-create EVERY row, empty
//     for (q = 0..n)
//       for ((key, val) in emb[q])                  // ascending key (std::map)
//         remb[key][q] += val;
//
// The second loop writes remb[key][q], the transpose position of emb[q][key].
// Each (q,key) pair of emb is visited exactly once (emb's per-row keys are
// unique), so the `+=` lands on a fresh-or-absent slot and is really a STORE:
// remb[key][q] == emb[q][key]. So the VALUES are emb^T exactly, and transpose()
// reproduces them with no arithmetic at all -- hence no accumulation order to
// preserve. Measured on the pinned fixture: 14651/14651 cells, 0 value diffs.
//
// TWO THINGS THIS IS NOT, both measured rather than assumed:
//
//  (1) NOT a bare transpose in ROW SET. The first loop materialises a row for
//      every q in [0,n), so the legacy's remb has n keys; transpose() has only
//      the rows that receive a value. On the pinned fixture the measured
//      difference is 0 empty-but-present rows (every id occurs in some emb row),
//      but that is a property of THIS fixture -- a dataset where some id is
//      nobody's embedding key would make the difference non-zero. The core does
//      not synthesise empty rows: they carry no value, and F4 reads a missing row
//      and an empty row identically (both 0.0f). The gate reports the count so
//      the difference stays visible if a future fixture changes it.
//
//  (2) THE COUNTERPART-ASYMMETRY IS THE REAL STRUCTURAL FACT, and it is F4's
//      problem, not F3's. Measured: 8623 of the 14651 remb cells (59%) have NO
//      emb[q][col] counterpart. That is emb being strongly ASYMMETRIC -- and it
//      means combineReverseEmb multiplies 59% of its entries by an absent (hence
//      operator[]-inserted 0.0f) reverse. F3 itself is unaffected: it never asks
//      whether a counterpart exists.
void f3_create_reverse_emb() {
  // Values: emb^T, exactly. transpose() copies each cell once with no
  // arithmetic, so there is no accumulation order to preserve.
  remb_ = emb_.transpose();
}

  // ------------------------------------------------------------------ F4 --
// combineReverseEmb (RFE.cpp:634-642). The legacy is:
//
//     for (q = 0..n) {
//       std::map<int,float> qemb = emb[q];        // COPY: iterate the old values
//       for ((key, val) in qemb)                   // ascending key
//         emb[q][key] = emb[q][key] * remb[q][key];
//     }
//
// WHY THIS IS NOT combine() AND NOT merge() -- both MEASURED, Step 1 H4b:
//   CSRMatrix::combine returns the intersection of the NONZERO supports
//   (SparseMatrix.hpp:721-772): a cell whose reverse partner is 0 or absent is
//   simply NOT EMITTED. The legacy keeps the KEY and writes 0.0f.
//   CSRMatrix::merge covers the union but filters `if (res != T{})` on emission
//   (SparseMatrix.hpp:675/682/701), so the same cells are dropped.
//   Measured on the pinned fixture: BOTH lost 3443 of 5461 keys (63%).
//   F3 measured WHY: emb is strongly asymmetric -- 8623 of 14651 remb cells
//   (59%) have no emb[q][col] counterpart -- so this is not a rare corner, it
//   is the majority case. Any engine kernel that drops zero-valued results is
//   therefore structurally wrong here, not merely imprecise.
//
// The arithmetic itself is trivial (one multiply per cell, no accumulation), so
// there is no summation order to preserve: `a * b` is commutative and
// associative-exact for a single operation. The WHOLE difficulty is the support.
//
// Support semantics reproduced exactly:
//   * every cell of emb is visited, so the support is UNCHANGED (H4 measured
//     5461 -> 5461);
//   * the reverse value is looked up in remb, which is emb^T (F3), so
//     remb.get(q, key) == emb.get(key, q);
//   * a missing reverse yields 0.0f via get() -- the same value the legacy's
//     std::map operator[] inserts. The KEY stays, carrying 0.0f.
//   * NO |v| filter on emission: the zeros are load-bearing (H2, and H8c, where
//     26.56% of incElems cells are exactly zero because of them).
void f4_combine_reverse_emb() {
  const int nt = omp_get_max_threads();
  std::vector<M::PaddedAllocator> alloc;
  alloc.reserve((std::size_t)nt);
  for (int i = 0; i < nt; ++i) alloc.emplace_back();
  std::vector<std::pair<idx_t, idx_t>> ranges((std::size_t)nt);
  const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

  // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
  #pragma omp parallel for schedule(static) num_threads(nt)
  for (int c = 0; c < nt; ++c) {
    const int tid = omp_get_thread_num();
    auto& a = *alloc[(std::size_t)tid].ptr;
    const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
    const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
    ranges[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};

    for (std::size_t q = r0; q < r1; ++q) {
      // Iterate emb's cells in ascending column order (for_each_in_row's
      // contract) and emit exactly one record per cell -- including the zeros.
      // Rows are independent, so the emission order is free and each thread's
      // records are already column-ascending, which lets the builder's V4 fast
      // path skip the per-row sort with nothing to coalesce.
      std::vector<Triplet> row;
      emb_.for_each_in_row((idx_t)q, [&](idx_t col, float v) {
        Triplet t;
        t.row = (idx_t)q;
        t.col = col;
        t.value = v * remb_.get((idx_t)q, col);
        t.is_set = false;
        row.push_back(t);
      });
      for (const Triplet& t : row) {
        Triplet* e = a.allocate();
        *e = t;
      }
    }
  }
  M out((idx_t)n_, (idx_t)n_, 0);
  M::build_from_partial_results(out, alloc, ranges);
  emb_ = std::move(out);   // emb is REPLACED, as in the legacy
}

  // ------------------------------------------------------------------ F5 --
  // sortEmb (RFE.cpp:469-489) + effectEstimationEmb (RFE.cpp:513-525, 527-550).
  //
  // THE LOAD-BEARING CONSTRAINT HERE IS std::sort INSTABILITY, and it is a
  // different kind of constraint from F2's.
  //
  //   tag_sort (RFE.cpp:513-521) sorts the index permutation [0..n) by
  //   ee[i] DESCENDING with std::sort. std::sort is NOT stable, so the relative
  //   order of EQUAL-valued ee entries is whatever libstdc++'s introsort
  //   happens to leave behind. That order becomes `sorted_ee`, which is the
  //   OUTER LOOP ORDER of F6's Gram accumulation (RFE.cpp:559, `for (int q :
  //   sorted_ee)`). Two entries with equal ee can therefore have DIFFERENT Gram
  //   contributions, because their emb rows differ. So the tie-break is not a
  //   cosmetic detail -- it is part of the output.
  //
  // CONSEQUENCE: this phase calls std::sort ITSELF, with the legacy's exact
  // comparator over the legacy's exact input sequence, rather than delegating to
  // a stable sort or to any engine kernel. Stable would be WRONG (it would not
  // reproduce the legacy's permutation) and a different algorithm would be a
  // different (and unjustified) tie-break. Reproducing libstdc++'s std::sort on
  // the same inputs is the only bit-exact option, and it is stable across thread
  // counts because the sort is single-threaded and input-ordered.
  //
  // sortEmb (RFE.cpp:497-510) has the same shape: std::sort over
  // vector<pair<int,float>> descending by .second, then the KEYS are kept in
  // that order. The legacy builds A by iterating emb[q], which for a CSR row is
  // ASCENDING COLUMN ORDER -- so the input sequence matches for_each_in_row's,
  // which is what makes the std::sort tie-break reproduce.
  void f5_sort_emb() {
    // NEGATIVE CONTROL, bench-only, OFF by default. The tie population is a
    // COUNT, not an EXPOSURE: the F5 tie-break only reaches the Gram if two rows
    // tied in `ee` contribute NON-ZERO values to the same cell in different
    // orders. Summing 0.0f is exact and commutative, so a field of exact zeros
    // cannot expose it at all. Toggling to stable_sort is the only way to find
    // out whether a real dataset exposes it -- and if this changes nothing, the
    // honest claim is "ties exist, exposure undemonstrated", not "covered".
    const bool stable_probe = f5sort_stable_;
    const int nt = omp_get_max_threads();
    sorted_emb_.assign((std::size_t)n_, std::vector<int>());
    const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

    // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      // Per-thread scratch, reused: the legacy allocates a fresh vector per row,
      // but capacity reuse cannot change std::sort's result.
      std::vector<std::pair<int, float>> A;
      for (std::size_t q = r0; q < r1; ++q) {
        A.clear();
        // Ascending column order == the legacy's std::map iteration order. The
        // input SEQUENCE to std::sort is what pins its tie-break, so this is
        // load-bearing and not merely cosmetic.
        emb_.for_each_in_row((idx_t)q, [&](idx_t col, float v) {
          A.emplace_back((int)col, v);
        });
        // The legacy's CmpDesc: strictly greater, descending. Using the same
        // comparator (not >=) is what makes the unstable sort land the same way.
        // The legacy's CmpDesc is strictly greater, descending. Using the same
        // comparator (not >=) is what makes the UNSTABLE sort land the same way;
        // the probe swaps only the ALGORITHM, never the comparator.
        if (stable_probe) {
          std::stable_sort(A.begin(), A.end(),
                           [](const std::pair<int, float>& a,
                              const std::pair<int, float>& b) {
                             return a.second > b.second;
                           });
        } else {
          std::sort(A.begin(), A.end(),
                    [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                      return a.second > b.second;
                    });
        }
        auto& out = sorted_emb_[(std::size_t)q];
        out.reserve(A.size());
        for (const auto& p : A) out.push_back(p.first);
      }
    }
  }

  // effectEstimationEmb (RFE.cpp:527-548).
  //
  //   ee[q] = sum over the FIRST k keys of sorted_emb[q] of emb[q][key]
  //   sorted_ee = tag_sort(ee)                 // unstable, see above
  //   grValue   = ee[sorted_ee[0]]              // the MAX, BEFORE normalisation
  //   ee[q]    /= grValue                       // divides by zero if grValue==0
  //
  // The summation is over the first k keys IN sorted_emb ORDER, so its order is
  // fixed once f5_sort_emb has reproduced that order. `emb[q][key]` is a
  // std::map operator[] lookup of a key that came FROM emb[q], so it always
  // hits; get() is the value-equivalent read (no spurious 0.0f insertion).
  void f5_effect_estimation_emb() {
    ee_.assign((std::size_t)n_, 0.0f);
    const int nt = omp_get_max_threads();
    const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

    // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      for (std::size_t q = r0; q < r1; ++q) {
        float eeValue = 0;
        int pos = 0;
        for (const int key : sorted_emb_[(std::size_t)q]) {
          pos++;
          if (pos > k_) break;          // first k keys only
          eeValue += emb_.get((idx_t)q, (idx_t)key);
        }
        ee_[(std::size_t)q] = eeValue;
      }
    }

    // tag_sort: the legacy's std::sort over an index permutation, descending,
    // UNSTABLE. Kept as the same std::sort call on the same input.
    sorted_ee_.resize((std::size_t)n_);
    for (int i = 0; i < n_; ++i) sorted_ee_[(std::size_t)i] = i;
    const std::vector<float>& v = ee_;
    std::sort(sorted_ee_.begin(), sorted_ee_.end(),
              [&v](const int lhs, const int rhs) {
                return v[(std::size_t)lhs] > v[(std::size_t)rhs];
              });

    // grValue is read BEFORE the normalisation loop divides by it. If it is 0
    // the legacy produces inf/nan here; kept verbatim rather than guarded,
    // because guarding would change the output on exactly the inputs where the
    // legacy is already degenerate.
    const int key = sorted_ee_[0];
    const float grValue = ee_[(std::size_t)key];
    gr_value_ = grValue;
    #pragma omp parallel for schedule(static)
    for (int q = 0; q < n_; ++q)
      ee_[(std::size_t)q] = ee_[(std::size_t)q] / grValue;
  }

  // ----------------------------------------------------------------- F7a --
  // reRankingByEmbSim (RFE.cpp:648-661) -- the t-LOOP phase, and the phase that
  // was MISSING from the F2..F6 chain.
  //
  //   for (q) { pos = 1; for (i < l) {
  //     img_i = rkLists[l*q + i]
  //     MX(q, img_i) = simEmb(emb[q], emb[img_i]) / pos;   pos += 1;
  //   } }
  //   execSortRankedLists();
  //
  // WHY THIS MATTERS MORE THAN ITS SIZE SUGGESTS. reRankingByEmbSim REWRITES
  // rkLists_, and execCartesianProduct then rebuilds emb from the REWRITTEN
  // lists. The Step 0 golden measures exactly this at the pinned fixture:
  //
  //   rankNorm       ck_rk=0x3465e556d414ca79
  //   embRR[0]       ck_rk=0xad0c4fe24904d9d9   emb_keys=14651
  //   embRR[1]       ck_rk=0xe86514a3dd0922a9   emb_keys=12812
  //   cartesianProd  ck_rk=0x58ded033b72ded72   emb_keys=12227
  //
  // so the emb that feeds F5/F6 is the 12227-key one, NOT the 14651-key one a
  // F1..F6-only chain produces. The earlier phases are still bit-exact as
  // measured -- but they were measured on a chain that OMITS this phase, so the
  // end-to-end pipeline could not have matched even with every phase individually
  // correct. The end-to-end check added with F7 is what closes that gap.
  void f7a_rerank_by_emb_sim() {
    const int nt = omp_get_max_threads();
    std::vector<M::PaddedAllocator> alloc7a;
    alloc7a.reserve((std::size_t)nt);
    for (int i = 0; i < nt; ++i) alloc7a.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges7a((std::size_t)nt);
    const std::size_t chunk7a = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;
    // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      auto& a = *alloc7a[(std::size_t)tid].ptr;
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk7a);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk7a);
      ranges7a[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};
      std::vector<std::pair<idx_t, float>> ra, rb;
      for (std::size_t q = r0; q < r1; ++q) {
      // Materialise row q ONCE: it is the left operand of every pair in this
      // row. H1b bounds a row at k + k^2 cells, independent of n, so this is
      // cheap and keeps the memory O(k^2) rather than O(nnz).
      ra.clear();
      emb_.for_each_in_row((idx_t)q, [&](idx_t c, float v) { ra.emplace_back(c, v); });
      for (int i = 0; i < l_; ++i) {
        const int img = rk_[(std::size_t)l_ * q + (std::size_t)i];
        rb.clear();
        emb_.for_each_in_row((idx_t)img, [&](idx_t c, float v) { rb.emplace_back(c, v); });
        const float sim = merge_sim(ra, rb);
        Triplet* t = a.allocate();
        t->row = (idx_t)q;
        t->col = (idx_t)img;
        // pos starts at 1 and increments AFTER the store, so slot i divides by
        // i+1. int pos promotes to float for the division.
        t->value = sim / (float)(i + 1);
        t->is_set = false;   // img distinct within a row: a rank list is a
                             // permutation, so exactly l records, no duplicates
      }
    }
  }
  M s((idx_t)n_, (idx_t)n_, 0);
  M::build_from_partial_results(s, alloc7a, ranges7a);
  sim_ = std::move(s);
  sort_ranked_lists();   // kernelSortRankedLists: stable descending + self pin
  }

  // simEmb (RFE.cpp:677-686): iterate di ASCENDING, accumulate v_i*v_j only for
  // keys dj also has. Both rows are CSR-ascending by column, so a two-pointer
  // merge replays the legacy's sequence exactly (H5 measured ZERO ULP).
  //
  // Both operands are materialised rather than probed with get(), and that is
  // deliberate: get() returns 0.0f for an ABSENT key, so accumulating v*0.0f
  // would differ from the legacy's `if (found)` on the first term -- 0.0f + 0.0f
  // is fine, but the legacy SKIPS the add entirely, and reproducing "skip" via
  // an arithmetic no-op is exactly the kind of rewrite this port does not do.
  // M1: skip non-finite sim values (bad_sim) to avoid propagating NaN/Inf
  static float merge_sim(const std::vector<std::pair<idx_t, float>>& a,
                           const std::vector<std::pair<idx_t, float>>& b) {
    float sim = 0;
    std::size_t ib = 0;
    for (const auto& kv : a) {
      while (ib < b.size() && b[ib].first < kv.first) ++ib;
      if (ib < b.size() && b[ib].first == kv.first) {
        float prod = kv.second * b[ib].second;
        if (std::isfinite(prod)) sim += prod;  // M1: skip non-finite products
      }
    }
    return sim;
  }
  // The Gram + incElems (reRankingByCPRR's core, RFE.cpp:552-581, via
  // incSimByECP at RFE.cpp:696-715).
  //
  //   for (q : sorted_ee)                    // ORDER IS THE OUTPUT, see below
  //     wi = ee[q]; wj = 1                   // literal 1 at the call site
  //     for (i : sorted_emb[q])              // guard: skip if i not in emb[q]
  //       vi = emb[q][i]
  //       for (j : sorted_emb[q])            // guard: skip if j not in emb[q]
  //         vj = emb[q][j]
  //         incElems[i].insert(j)            // FIRES REGARDLESS OF VALUE
  //         MX(i,j) += (vi*vj) * wi * wj
  //
  // WHY THIS CANNOT BE PARALLELISED OVER q. MX(i,j) is `+=`-accumulated once per
  // q, so a cell's partial sums depend on the ORDER of the q loop. That order is
  // sorted_ee -- which F5 established is a libstdc++-unstable tie-break -- so a
  // parallel reduction over q would sum the same terms in a different order and
  // produce a different float. Bit-exactness forbids it.
  //
  // THE KERNEL THEREFORE PARALLELISES OVER ROWS i, NOT OVER q. Rows are
  // independent (row i only ever writes MX(i,*)), and within a row the q loop
  // still runs in sorted_ee order, so every cell sees the legacy's exact
  // summation sequence. This is the same re-association-avoidance as F2, one
  // level up: F2 fixed the order inside a row, F6 fixes it across q.
  //
  // CANDIDATE DISCOVERY IS SEPARATE FROM ACCUMULATION, and it must be. The
  // legacy discovers incElems[i] and accumulates MX(i,j) in the SAME pass, but
  // the port cannot: a CSR row's column set must be known before the row can be
  // emitted. So pass 1 builds the candidate set
  //     incElems[i] = union over {q : i in sorted_emb[q]} of sorted_emb[q]
  // and pass 2 accumulates into it. Splitting is sound precisely because
  // `insert` is idempotent and value-independent -- which is exactly why the
  // union is well defined.
  //
  // THE ZERO CELLS ARE THE POINT. incElems[i].insert(j) fires even when
  // vi*vj*wi*wj == 0.0f exactly (H8: 26.56% of incElems cells at the pinned
  // fixture). Those are real candidates carrying a real zero Gram value, and they
  // must survive into the CSR with their zeros intact. NO |v| filter, and no
  // combine()/merge() on the way in -- both were measured dropping keys (F4).
  void f6_gram_and_inc_elems() {
    const bool prof = f6prof_.enabled;
    const double t_f6_0 = prof ? omp_get_wtime() : 0.0;
    f6prof_.threads = omp_get_max_threads();
    const double t_inv_0 = prof ? omp_get_wtime() : 0.0;
    build_emb_inverted_by_sorted_ee();
    const double t_inv_1 = prof ? omp_get_wtime() : 0.0;

    const int nt = omp_get_max_threads();
    const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

    std::vector<RowAcc> accs((std::size_t)nt);
    std::vector<M::PaddedAllocator> alloc;
    alloc.reserve((std::size_t)nt);
    for (int i = 0; i < nt; ++i) alloc.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges((std::size_t)nt);
    // Per-thread profile slots. Per-thread, NOT atomics and NOT a shared
    // reduction: the sums are of DISJOINT row ranges, so a flat sum after the
    // region is exact and race-free.
    std::vector<double> p_disc((std::size_t)nt, 0.0), p_sort((std::size_t)nt, 0.0),
                        p_acc((std::size_t)nt, 0.0), p_emit((std::size_t)nt, 0.0);
    std::vector<long long> p_raw((std::size_t)nt, 0), p_uniq((std::size_t)nt, 0),
                          p_inn((std::size_t)nt, 0);

    // Refactored to parallel for over chunks (H3: robust to OMP_DYNAMIC/THREAD_LIMIT)
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const int tid = omp_get_thread_num();
      auto& a = *alloc[(std::size_t)tid].ptr;
      RowAcc& acc = accs[(std::size_t)tid];
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      ranges[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};

      for (std::size_t i = r0; i < r1; ++i) {
        acc.clear();
        // ---- pass 1: candidate discovery (the union) ----
        // qs_ is ascending in sorted_ee POSITION, i.e. the legacy's q order.
        const std::vector<std::uint32_t>& qs = inv_of_col_[(idx_t)i];
        const double ta = prof ? omp_get_wtime() : 0.0;
        for (std::uint32_t t : qs) {
          const int q = sorted_ee_[(std::size_t)t];
          for (const int j : sorted_emb_[(std::size_t)q]) acc.add_candidate((idx_t)j);
        }
        const double tb = prof ? omp_get_wtime() : 0.0;
        // Counted HERE, before finalize_candidates() resizes cols down to the
        // distinct set -- reading cols.size() after it would silently report the
        // uniq count for BOTH fields and make raw/uniq a tautological 1.00.
        if (prof) p_raw[(std::size_t)tid] += (long long)acc.cols.size();
        // Sort + dedupe HERE, between the passes: pass 2 binary-searches, so the
        // column set must be sorted and unique before any value is added.
        acc.finalize_candidates();
        const double tc = prof ? omp_get_wtime() : 0.0;
        if (prof) p_uniq[(std::size_t)tid] += (long long)acc.size();
        // ---- pass 2: accumulation, q visited in sorted_ee order ----
        for (std::uint32_t t : qs) {
          const int q = sorted_ee_[(std::size_t)t];
          const float wi = ee_[(std::size_t)q];
          const float vi = emb_.get((idx_t)q, (idx_t)i);
          for (const int j : sorted_emb_[(std::size_t)q]) {
            const float vj = emb_.get((idx_t)q, (idx_t)j);
            // The legacy's expression, term for term: (vi*vj) * wi * wj with wj
            // the literal 1 at the call site. Written as written so the float ops
            // match; the *1 is KEPT because dropping it would be a rewrite, not a
            // simplification, and this port does not rewrite float expressions.
            acc.add((idx_t)j, (vi * vj) * wi * 1.0f);
          }
        }
        const double td = prof ? omp_get_wtime() : 0.0;
        if (prof) {
          for (std::uint32_t t : qs) p_inn[(std::size_t)tid] +=
              (long long)sorted_emb_[(std::size_t)sorted_ee_[(std::size_t)t]].size();
        }
        // Columns are already ascending and unique after finalize_candidates(),
        // and pass 2 never inserts, so the emission order is strictly ascending:
        // the builder's V4 fast path applies with nothing to coalesce.
        for (std::size_t s = 0; s < acc.size(); ++s) {
          Triplet* t = a.allocate();
          t->row = (idx_t)i;
          t->col = acc.col(s);
          t->value = acc.val(s);
          t->is_set = false;
        }
        const double te = prof ? omp_get_wtime() : 0.0;
        if (prof) {
          p_disc[(std::size_t)tid] += tb - ta;
          p_sort[(std::size_t)tid] += tc - tb;
          p_acc[(std::size_t)tid]  += td - tc;
          p_emit[(std::size_t)tid] += te - td;
        }
      }
    }
    if (prof) {
      f6prof_.t_inv = t_inv_1 - t_inv_0;
      f6prof_.t_disc = f6prof_.t_sort = f6prof_.t_acc = f6prof_.t_emit = 0.0;
      f6prof_.cand_raw = f6prof_.cand_uniq = f6prof_.inner_iters = 0;
      for (int i = 0; i < nt; ++i) {
        f6prof_.t_disc += p_disc[(std::size_t)i];
        f6prof_.t_sort += p_sort[(std::size_t)i];
        f6prof_.t_acc  += p_acc[(std::size_t)i];
        f6prof_.t_emit += p_emit[(std::size_t)i];
        f6prof_.cand_raw    += p_raw[(std::size_t)i];
        f6prof_.cand_uniq   += p_uniq[(std::size_t)i];
        f6prof_.inner_iters += p_inn[(std::size_t)i];
        if (p_disc[(std::size_t)i] > f6prof_.m_disc) f6prof_.m_disc = p_disc[(std::size_t)i];
        if (p_sort[(std::size_t)i] > f6prof_.m_sort) f6prof_.m_sort = p_sort[(std::size_t)i];
        if (p_acc [(std::size_t)i] > f6prof_.m_acc ) f6prof_.m_acc  = p_acc[(std::size_t)i];
        if (p_emit[(std::size_t)i] > f6prof_.m_emit) f6prof_.m_emit = p_emit[(std::size_t)i];
      }
    }
    const double t_build_0 = prof ? omp_get_wtime() : 0.0;
    M gram((idx_t)n_, (idx_t)n_, 0);
    M::build_from_partial_results(gram, alloc, ranges);
    gram_ = std::move(gram);
    if (prof) f6prof_.t_build = omp_get_wtime() - t_build_0;

    if (prof) {
      // Load balance of the row split: max/min |inv_of_col_[i]|. This is what
      // bounds F6's parallel efficiency -- rows are independent but NOT
      // equal-cost, and the chunking is static.
      f6prof_.inv_min = (long long)n_;
      f6prof_.inv_max = 0;
      for (std::size_t i = 0; i < (std::size_t)n_; ++i) {
        const long long m = (long long)inv_of_col_[i].size();
        if (m < f6prof_.inv_min) f6prof_.inv_min = m;
        if (m > f6prof_.inv_max) f6prof_.inv_max = m;
      }
    }

    // incElems is the SAME support as the Gram rows by construction (pass 1
    // created exactly the candidates pass 2 filled), so it is read off gram_
    // rather than stored twice. incElemsL is the legacy's list-based set.
    if (prof) f6prof_.t_total = omp_get_wtime() - t_f6_0;
    build_inc_elems_l();
  }

  // Inverted index over columns: for each element i, the ascending list of
  // sorted_ee POSITIONS t such that i in sorted_emb[sorted_ee[t]]. Built by one
  // serial scan in t, so each list is already in the legacy's q order and F6's
  // inner loop needs no sort. This is the F1 position-index trick again, one
  // structure up: both replace a legacy O(n) scan with a prebuilt index.
  void build_emb_inverted_by_sorted_ee() {
    inv_of_col_.assign((std::size_t)n_, std::vector<std::uint32_t>());
    for (std::size_t t = 0; t < sorted_ee_.size(); ++t) {
      const int q = sorted_ee_[t];
      for (const int i : sorted_emb_[(std::size_t)q])
        inv_of_col_[(idx_t)i].push_back((std::uint32_t)t);
    }
  }

  // incElemsL[q] = the set of the first l entries of rkLists row q. The legacy
  // does `pos = 1; for (i < l) { pos++; insert(img_i); }` -- the pos++ is dead
  // (pos is never read again), so this is exactly the l entries; and since a rank
  // list is a permutation of distinct ids the set has l elements.
  void build_inc_elems_l() {
    inc_elems_l_.assign((std::size_t)n_, std::vector<int>());
    #pragma omp parallel for schedule(static)
    for (int q = 0; q < n_; ++q) {
      std::vector<int>& s = inc_elems_l_[(std::size_t)q];
      s.reserve((std::size_t)l_);
      for (int i = 0; i < l_; ++i)
        s.push_back(rk_[(std::size_t)l_ * q + (std::size_t)i]);
      std::sort(s.begin(), s.end());
      s.erase(std::unique(s.begin(), s.end()), s.end());
    }
  }

  // ----------------------------------------------------------------- F7b --
  // The CPRR TAIL: kernel_sort_ranked_lists_extra (RFE.cpp:769-833), driven by
  // set_difference(incElems[q], incElemsL[q]).
  //
  //   rkTmp = rkLists[q][0..l) ++ extra_elems      // extras ASCENDING by id
  //   a[j]  = MX(q, rkTmp[j])                     // incl. the pow(2,30) diagonal
  //   insertion sort ASCENDING (stable: shifts on strict `<`)
  //   pin self to position 0, then copy back only the first l entries
  //
  // THREE THINGS THAT ARE EASY TO GET WRONG, all found by measurement:
  //
  //  (1) THE SORT IS DESCENDING, and I got this wrong first. The legacy's loop is
  //      `while (i >= 0 && a[i] < keyA) shift-right`, which moves SMALLER values
  //      to HIGHER indices -- i.e. DESCENDING, not ascending. My first draft read
  //      the same loop as an ascending insertion sort and failed E2E on
  //      7800/8000 cells. The measured proof: the legacy's row-0 output carries
  //      values 31.01, 16.81, 11.90, 4.16, 2.81, 2.59, 1.95 -- monotonically
  //      DECREASING. The loop is also STABLE (it shifts on strict `<` only), so
  //      std::stable_sort on `>` reproduces it, with ties keeping rkTmp order
  //      (current list first, then extras ascending by id).
  //
  //  (2) THE DIAGONAL IS PINNED, so self wins BY VALUE, not by convention.
  //      MX(q,q) = pow(2,30) (RFE.cpp:605) is applied at READ time here.
  //      gram_.get(q,q) returns the ACCUMULATED diagonal instead, so without the
  //      pin self sorts by its real (small) score and lands mid-list. Under a
  //      DESCENDING sort the pin also makes self land at position 0 naturally,
  //      which is why the explicit self-swap below is a no-op in practice -- it
  //      is kept because the legacy performs it unconditionally.
  //
  //  (3) NO TOP-L PRUNE, DELIBERATELY. See union_prune_row() below: the prune
  //      exists as a separate, separately-tested routine, and F7b does not call
  //      it, so the parity claim stays unconditional.
  void f7b_rerank_cprr_tail() {
    // Recorded per row, thrown after the region -- see the note at the throw site.
    std::vector<char> missing_self((std::size_t)n_, 0);
    #pragma omp parallel for schedule(static)
    for (int q = 0; q < n_; ++q) {
      // set_difference(incElems[q], incElemsL[q]). Both ascending -- the Gram row
      // is CSR-ascending and incElemsL[q] was sorted -- so this is a linear merge,
      // not the legacy's std::set_difference over two std::sets.
      std::vector<int> extras;
      std::vector<float> vals;
      gram_.for_each_in_row((idx_t)q, [&](idx_t c, float v) {
        extras.push_back((int)c);
        vals.push_back(v);
      });
      const std::vector<int>& L = inc_elems_l_[(std::size_t)q];
      std::size_t w = 0;
      {
        std::size_t il = 0;
        for (std::size_t s = 0; s < extras.size(); ++s) {
          const int key = extras[s];
          while (il < L.size() && L[il] < key) ++il;
          if (il < L.size() && L[il] == key) continue;   // present in incElemsL
          extras[w] = key;
          vals[w] = vals[s];
          ++w;
        }
        extras.resize(w);
        vals.resize(w);
      }

      const std::size_t cap = (std::size_t)l_ + extras.size();
      std::vector<int> rkTmp(cap);
      std::vector<float> a(cap);
      for (int i = 0; i < l_; ++i) {
        const int img = rk_[(std::size_t)l_ * q + (std::size_t)i];
        rkTmp[(std::size_t)i] = img;
        a[(std::size_t)i] = (img == q) ? diag_pin_ : gram_.get((idx_t)q, (idx_t)img);
      }
      for (std::size_t e = 0; e < extras.size(); ++e) {
        rkTmp[(std::size_t)l_ + e] = extras[e];
        a[(std::size_t)l_ + e] = (extras[e] == q) ? diag_pin_ : vals[e];
      }

      // M2: guarda não-finito no F7b - check for non-finite values before sorting
      for (std::size_t s = 0; s < cap; ++s) {
        if (!std::isfinite(a[s])) {
          a[s] = 0.0f;  // M2: replace non-finite with 0.0f to avoid NaN/Inf propagation
        }
      }
      std::vector<std::size_t> ord(cap);
      for (std::size_t s = 0; s < cap; ++s) ord[s] = s;
      std::stable_sort(ord.begin(), ord.end(),
                       [&a](std::size_t x, std::size_t y) { return a[x] > a[y]; });
      std::vector<int> sorted(cap);
      for (std::size_t s = 0; s < cap; ++s) sorted[s] = rkTmp[ord[s]];

      // Self pinned to 0. The legacy scans `rkTmp[i] != rk` BEFORE testing
      // i < new_size (RFE.cpp:816), so a row without self reads out of bounds.
      // Self is present by the dataset convention and the gate's self0 check
      // enforces it; fail fast rather than read out of bounds.
      std::size_t self_pos = sorted.size();
      for (std::size_t s = 0; s < sorted.size(); ++s) {
        if (sorted[s] == q) { self_pos = s; break; }
      }
      if (self_pos == sorted.size()) {
        // RECORD, do not throw: this is inside `#pragma omp parallel for`, and a
        // throw from an OpenMP region is undefined behaviour -- libgomp calls
        // std::terminate, so the caller never sees the exception. Caught by
        // test_rfe_corner.cpp. The throw is hoisted below the loop.
        missing_self[(std::size_t)q] = 1;
        continue;
      }
      if (self_pos > 0) std::swap(sorted[0], sorted[self_pos]);

      for (int i = 0; i < l_; ++i)
        rk_[(std::size_t)l_ * q + (std::size_t)i] = sorted[(std::size_t)i];
    }
    // Outside the parallel region, where a throw actually propagates.
    for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
      if (missing_self[q])
        throw std::runtime_error(
            "RFE: row " + std::to_string(q) + " does not contain its own id; the "
            "legacy reads out of bounds in kernelSortRankedLists_extra (RFE.cpp:816)");
    }
  }

  // H8's union prune, as a SEPARATE, SEPARATELY-TESTED routine. It is NOT called
  // by f7b_rerank_cprr_tail: the legacy keeps every candidate and takes the first
  // l after sorting, so pruning is an OPTIMISATION that must prove it never
  // changes the first l. Keeping it out of the parity path means the parity claim
  // does not depend on that proof.
  //
  // The candidate set is the UNION of (nonzero Gram) and (zero-valued incElems).
  // H8 measured that a VALUE-ONLY prune is NOT exact: 15-27% of incElems cells
  // carry exactly 0.0f yet still discover a candidate (RFE.cpp:710), so filtering
  // on `v != 0` would drop real candidates. There is deliberately NO such filter.
  //
  // THE TIE ORDER IS THE WHOLE SUBTLETY, and my first version got it wrong. The
  // legacy's rkTmp is [current list in ITS OWN ORDER] ++ [extras ascending by id],
  // and the stable sort preserves that order within a tie group. Ranking ties by
  // ascending id INSTEAD is wrong, because the current list is not in ascending
  // id order -- measured as 3 of 200 rows differing. So this reuses the very same
  // rkTmp construction f7b uses, and differs only in not writing back.
  std::vector<int> union_prune_row(int q, std::size_t top_l) const {
    std::vector<int> extras;
    std::vector<float> vals;
    gram_.for_each_in_row((idx_t)q, [&](idx_t c, float v) {
      extras.push_back((int)c);
      vals.push_back(v);
    });
    const std::vector<int>& L = inc_elems_l_[(std::size_t)q];
    std::size_t w = 0;
    {
      std::size_t il = 0;
      for (std::size_t s = 0; s < extras.size(); ++s) {
        const int key = extras[s];
        while (il < L.size() && L[il] < key) ++il;
        if (il < L.size() && L[il] == key) continue;
        extras[w] = key;
        vals[w] = vals[s];
        ++w;
      }
      extras.resize(w);
      vals.resize(w);
    }
    const std::size_t cap = (std::size_t)l_ + extras.size();
    std::vector<std::size_t> ord(cap);
    for (std::size_t s = 0; s < cap; ++s) ord[s] = s;
    // Rank by value, descending; ties keep the rkTmp position, which is the
    // legacy's own order (current list first, then extras ascending by id).
    auto value_at = [&](std::size_t s) -> float {
      if (s < (std::size_t)l_) {
        const int img = rk_[(std::size_t)l_ * q + s];
        return (img == q) ? diag_pin_ : gram_.get((idx_t)q, (idx_t)img);
      }
      const std::size_t e = s - (std::size_t)l_;
      return (extras[e] == q) ? diag_pin_ : vals[e];
    };
    std::stable_sort(ord.begin(), ord.end(),
                     [&value_at](std::size_t x, std::size_t y) {
                       return value_at(x) > value_at(y);
                     });
    std::vector<int> out;
    for (std::size_t s = 0; s < cap && s < top_l; ++s) {
      const std::size_t pos = ord[s];
      out.push_back(pos < (std::size_t)l_
                        ? rk_[(std::size_t)l_ * q + pos]
                        : extras[pos - (std::size_t)l_]);
    }
    return out;
  }

// H4 + H19 (option a): validate input at the START of run() -- O(N*L)
// fail-fast before any computation. Checks: all ids in [0, n), each row has
// exactly l distinct ids, and each row CONTAINS its own id at ANY position
// (self at index 0 is NOT required -- H19 option (a): the legacy pins self
// during the F1 kernel sort, so the entry contract is "contains self").
// Rows with self off index 0 are counted (self_off0_) and logged.
void validate_input() {
  // Check all ids in [0, n) and each row has exactly l entries
  std::vector<char> seen((std::size_t)n_, 0);
  self_off0_ = 0;
  for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
    std::size_t base = (std::size_t)l_ * q;
    bool has_self = false;
    for (int i = 0; i < l_; ++i) {
      int v = rk_[base + (std::size_t)i];
      if (v < 0 || v >= n_) {
        throw std::runtime_error("RFE: row " + std::to_string(q) + " has id " + std::to_string(v) + " out of range [0, " + std::to_string(n_) + ")");
      }
      if (seen[(std::size_t)v]) {
        throw std::runtime_error("RFE: row " + std::to_string(q) + " has duplicate id " + std::to_string(v));
      }
      seen[(std::size_t)v] = 1;
      if (v == (int)q) has_self = true;
    }
    if (!has_self)
      throw std::runtime_error(
          "RFE: row " + std::to_string(q) +
          " does not contain its own id; the legacy reads out of bounds at "
          "this point (RFE.cpp:754-756)");
    if (rk_[base] != (int)q) ++self_off0_;
    // Reset seen for next row
    for (int i = 0; i < l_; ++i) {
      seen[(std::size_t)rk_[base + (std::size_t)i]] = 0;
    }
  }
  // Controlled log (Etapa 6b item 8): fires only under RFE_RKSELF_OFF0, matching
  // the codebase's getenv-probe pattern (RFE_F1PROF, PHASE_BUILDER). The counter
  // self_off0() is always updated; only the PRINT is gated, so run() no longer
  // pollutes stdout on every call. The selfoff0 gate sets the env var.
  if (std::getenv("RFE_RKSELF_OFF0")) {
    std::printf("RKSELF_OFF0 rows=%zu off0=%lld (accepted; legacy pins self during the F1 sort)\n",
                (std::size_t)n_, self_off0_);
  }
}

  // THE ENTRY POINT. This is the ONLY pipeline the port is allowed to run, and
  // the parity gate calls THIS rather than spelling the sequence out again.
  //
  // X9: the gate used to reimplement the sequence inside test_rfe_parity, and a
  // COPY can drift from the thing it is checking without anything failing. It
  // did: this function skipped f7a_rerank_by_emb_sim entirely and ran the embRR
  // block once instead of t_ times, and the gate stayed green because the gate
  // never called it. A gate must exercise the production entry point.
  //
  // Mirrors RFE::runUdlMethod() (RFE.cpp:102-124) restricted to the in-scope
  // core. The legacy's tail -- if (performCCs) execRRCCs(); and
  // if (rerankByEmb || exportEmbeddings) execRRByEmbCCs(); -- is NOT fully
  // ported. RERANK_BY_EMB / EXPORT_EMBEDDINGS are refused by the carrier's
  // preprocess() (RFE.hpp:92-96). PERFORM_CCS is refused HERE, at the top of
  // run(), rather than being skipped: silently ignoring it would emit different
  // rank lists than the legacy. Etapa 9 replaces this throw with the real
  // post-f7b CC sequence.
  void run() {
    // H4: validate input at the START of run() -- O(N*L) fail-fast before any computation
    validate_input();

    // The profile lives HERE, beside the sequence, not in the bench. A profile
    // outside run() would measure its own copy -- which is the defect X9 names,
    // and it is exactly how the pre-t-loop "full pipeline" numbers happened.
    const bool prof = runprof_.enabled;
    if (prof) runprof_.begin(t_, n_, l_);
    const double R0 = prof ? omp_get_wtime() : 0.0;

    // Times a phase without changing the order. The branch is predictable and
    // the closure is inlined; when prof is false this is a direct call.
    auto tm = [prof](double& slot, auto&& fn) {
      if (!prof) { fn(); return; }
      const double s = omp_get_wtime();
      fn();
      slot += omp_get_wtime() - s;
    };

    tm(runprof_.t_f1, [this] { f1_rank_norm(); });
    if (prof) { runprof_.pk_f1 = (double)peak_kb(); reset_peak(); }
    for (int ct = 0; ct < t_; ++ct) {
      tm(runprof_.t_f2t, [this] { f2_create_emb(); });
      tm(runprof_.t_f3t, [this] { f3_create_reverse_emb(); });
      tm(runprof_.t_f4t, [this] { f4_combine_reverse_emb(); });
      // emb state ENTERING f7a -- that is what drives f7a's work, so it is
      // recorded here, before f7a rewrites the rank lists.
      if (prof) runprof_.record_emb(emb_, *this);
      tm(runprof_.t_f7at, [this] { f7a_rerank_by_emb_sim(); });
    }
    if (prof) { runprof_.pk_tloop = (double)peak_kb(); reset_peak(); }
    tm(runprof_.t_embrr, [this] {
      f2_create_emb(); f3_create_reverse_emb(); f4_combine_reverse_emb();
    });
    if (prof) { runprof_.pk_embrr = (double)peak_kb(); reset_peak(); }
    tm(runprof_.t_f5sort,  [this] { f5_sort_emb(); });
    tm(runprof_.t_f5eff,   [this] { f5_effect_estimation_emb(); });
    if (prof) { runprof_.pk_f5 = (double)peak_kb(); reset_peak(); }
    tm(runprof_.t_f6,      [this] { f6_gram_and_inc_elems(); });
    if (prof) { runprof_.pk_f6 = (double)peak_kb(); reset_peak(); }
    tm(runprof_.t_f7b,     [this] { f7b_rerank_cprr_tail(); });
    if (prof) runprof_.pk_f7b = (double)peak_kb();

    // Etapa 9: PERFORM_CCS integration -- post-F7b CC sequence
    if (performCCs_) {
      // 1. Release F1-F7b heavy memory (gram_, inv_of_col_, sim_, remb_).
      //    DOCUMENT: ck_gram()/nnz_sim() become invalid post-CC.
      if (prof) { reset_peak(); }
      gram_ = M();           // release memory via move-assignment
      inv_of_col_.clear(); inv_of_col_.shrink_to_fit();
      sim_ = M();
      remb_ = M();
      if (prof) { runprof_.pk_f8a = (double)peak_kb(); reset_peak(); }

      // 2. Recompute embeddings over POST-F7b rk_ (T1)
      tm(runprof_.t_f2t, [this] { f2_create_emb(); });
      tm(runprof_.t_f3t, [this] { f3_create_reverse_emb(); });
      tm(runprof_.t_f4t, [this] { f4_combine_reverse_emb(); });
      if (prof) { runprof_.pk_embrr_cc = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f5sort,  [this] { f5_sort_emb(); });
      tm(runprof_.t_f5eff,   [this] { f5_effect_estimation_emb(); });
      if (prof) { runprof_.pk_f5_cc = (double)peak_kb(); reset_peak(); }

      // 3. Rebuild indices on new rk_
      build_position_index();   // F1 index was pre-F7b
      build_inc_elems_l();      // F6 inc_elems_l_ was pre-F7b (legacy rebuilds at ref:1005-1010)
      if (prof) { runprof_.pk_idx_cc = (double)peak_kb(); reset_peak(); }

      // 4. F8a -> F8h
      tm(runprof_.t_f8a, [this] { f8a_create_edges_ccs(); });
      if (prof) { runprof_.pk_f8a = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f8b, [this] { f8b_init_th_cc(); });
      tm(runprof_.t_f8c, [this] { f8c_edge_prefix(); });
      tm(runprof_.t_f8d, [this] { f8d_process_ccs(); });
      if (prof) { runprof_.pk_f8d = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f8e, [this] { f8e_compute_ccs_emb(); });
      if (prof) { runprof_.pk_f8e = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f8f, [this] { f8f_compute_embs_by_ccs(); });
      if (prof) { runprof_.pk_f8f = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f8g, [this] { f8g_rerank_cells(); });
      if (prof) { runprof_.pk_f8g = (double)peak_kb(); reset_peak(); }
      tm(runprof_.t_f8h, [this] { f8h_rerank_tail(); });
      if (prof) { runprof_.pk_f8h = (double)peak_kb(); }
    }

    if (prof) runprof_.end(*this, omp_get_wtime() - R0);
  }

 public:
  // FASE 2. Per-phase TIMES plus per-iteration WORK COUNTS, so the cost model is
  // measured rather than predicted. The two F7a counters are derived from OUTSIDE
  // the parallel loop (a row-size table and the pair count), because a counter
  // inside it would need atomics and would perturb the very time it measures.
  struct RunProfile {
    // Peak VmHWM reached by the time each boundary is passed (clear_refs=5
    // before each phase).
    double pk_f1 = 0, pk_tloop = 0, pk_embrr = 0, pk_f5 = 0, pk_f6 = 0, pk_f7b = 0;
    double pk_embrr_cc = 0, pk_f5_cc = 0, pk_idx_cc = 0;
    double pk_f8a = 0, pk_f8b = 0, pk_f8c = 0, pk_f8d = 0, pk_f8e = 0, pk_f8f = 0, pk_f8g = 0, pk_f8h = 0;
    double t_f1 = 0, t_f2t = 0, t_f3t = 0, t_f4t = 0, t_f7at = 0;
    double t_embrr = 0, t_f5sort = 0, t_f5eff = 0, t_f6 = 0, t_f7b = 0, t_total = 0;
    double t_embrr_cc = 0, t_f5sort_cc = 0, t_f5eff_cc = 0, t_idx_cc = 0;
    double t_f8a = 0, t_f8b = 0, t_f8c = 0, t_f8d = 0, t_f8e = 0, t_f8f = 0, t_f8g = 0, t_f8h = 0;
    std::vector<long long> emb_nnz_it, emb_zeros_it, f7a_visits_it;
    long long emb_nnz_final = 0, emb_zeros_final = 0;
    long long gram_nnz = 0, gram_zeros = 0;
    double gram_rho = 0;
    // X6 applied to the INSTRUMENT. end() takes the core and fills every counter
    // above. Four of them (emb_nnz_final, emb_zeros_final, gram_nnz, gram_zeros)
    // plus gram_rho had NO writer at all for a while and reported a zero that
    // nobody had measured -- indistinguishable from "measured zero". This flag
    // separates the two, and the gate asserts the fields against the core's own
    // accessors, so the profiler cannot drift from what it profiles.
    bool fields_written = false;
    long long f7a_merges_total = 0;
    int t_loops = 0, threads = 1;
    bool enabled = false;

    void begin(int t, std::size_t n, std::size_t l) {
      t_f1 = t_f2t = t_f3t = t_f4t = t_f7at = 0;
      t_embrr = t_f5sort = t_f5eff = t_f6 = t_f7b = t_total = 0;
      emb_nnz_it.clear(); emb_zeros_it.clear(); f7a_visits_it.clear();
      emb_nnz_final = emb_zeros_final = gram_nnz = gram_zeros = 0;
      gram_rho = 0; f7a_merges_total = 0;
      emb_nnz_final = emb_zeros_final = gram_nnz = gram_zeros = 0;
      fields_written = false;
      t_loops = t; threads = omp_get_max_threads();
      pk_f1 = pk_tloop = pk_embrr = pk_f5 = pk_f6 = pk_f7b = 0;
      if (enabled) reset_peak();
      f7a_merges_total = (long long)t * (long long)n * (long long)l;
    }
    void record_emb(const M& emb, const RFECore& c) {
      emb_nnz_it.push_back((long long)emb.nnz());
      emb_zeros_it.push_back((long long)c.count_emb_zeros());
      f7a_visits_it.push_back(count_f7a_visits(emb, c));
    }
    void end(RFECore& c, double tot) {
      t_total = tot;
      emb_nnz_final   = (long long)c.emb_.nnz();
      emb_zeros_final = c.count_emb_zeros();
      gram_nnz        = (long long)c.gram_.nnz();
      gram_zeros      = c.count_gram_zeros();
      gram_rho        = c.n_ ? (double)gram_nnz / ((double)c.n_ * (double)c.n_) : 0.0;
      fields_written  = true;
    }
    // Sum over the (q,i) pairs of |emb[ img ]| -- the cell visits f7a performs
    // materialising the right operand. Computed from the CURRENT rank lists, so
    // it must be called before f7a rewrites them.
    static long long count_f7a_visits(const M& emb, const RFECore& c) {
      std::vector<std::size_t> sz((std::size_t)c.n_, 0);
      for (std::size_t q = 0; q < (std::size_t)c.n_; ++q) {
        std::size_t k = 0;
        emb.for_each_in_row((idx_t)q, [&](idx_t, float) { ++k; });
        sz[q] = k;
      }
      long long visits = 0;
      for (std::size_t q = 0; q < (std::size_t)c.n_; ++q)
        for (int i = 0; i < c.l_; ++i) {
          const int img = c.rk_[(std::size_t)c.l_ * q + (std::size_t)i];
          if (img >= 0 && (std::size_t)img < (std::size_t)c.n_) visits += (long long)sz[img];
        }
      return visits;
    }
  };
  RunProfile runprof_;

 public:
  long long count_emb_zeros() const {
    long long c = 0;
    for (idx_t r = 0; r < (idx_t)n_; ++r)
      emb_.for_each_in_row(r, [&](idx_t, float v) { if (v == 0.0f) ++c; });
    return c;
  }

  // Peak attribution by phase, using the repo's own precedent
  // (Data/phase_profile/etapa_a/README.md: echo 5 > /proc/self/clear_refs before
  // each build). clear_refs=5 resets the PEAK COUNTER only, never the memory, so
  // each reading is the peak REACHED up to that boundary. The phase with the
  // largest number is where the process peaked -- a single VmHWM says the peak
  // but not WHO reached it.
  static void reset_peak() {
    std::FILE* f = std::fopen("/proc/self/clear_refs", "w");
    if (f) { std::fputs("5\n", f); std::fclose(f); }
  }
  static long peak_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f))
      if (std::strncmp(line, "VmHWM:", 6) == 0) { kb = std::atol(line + 6); break; }
    std::fclose(f);
    return kb;
  }

  long long count_gram_zeros() const {
    long long c = 0;
    for (idx_t r = 0; r < (idx_t)n_; ++r)
      gram_.for_each_in_row(r, [&](idx_t, float v) { if (v == 0.0f) ++c; });
    return c;
  }

  void set_run_profile(bool on) { runprof_.enabled = on; }
  const RunProfile& run_profile() const { return runprof_; }

// --------------------------------------------------------------- F1 out --
  const M& sim() const { return sim_; }
  std::size_t nnz_sim() const { return sim_.nnz(); }

  // FNV-1a over the raw bytes of the rank lists -- the SAME instrument the
  // Step 0 golden uses, so the digests are directly comparable.
  std::uint64_t ck_rk() const { return fnv_bytes(rk_.data(), rk_.size() * sizeof(int)); }
  // FNV-1a over the CSR cells as (row, col, value_bits). The offset basis is the
  // golden's literal -- see fnv_bytes below for why it is NOT the canonical one.
  std::uint64_t ck_sim() const {
    std::uint64_t h = 1469598103934665603ULL;
    for (idx_t r = 0; r < (idx_t)n_; ++r) {
      sim_.for_each_in_row(r, [&](idx_t c, float v) {
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        std::uint64_t x = ((std::uint64_t)r << 32) ^ (std::uint64_t)c;
        h ^= x; h *= 1099511628211ULL;
        h ^= b; h *= 1099511628211ULL;
      });
    }
    return h;
  }
  // Digest of the DENSE view (implicit zeros included), so it matches the
  // golden's ck_matrix bit-for-bit. Used only by the parity gate.
  std::uint64_t ck_sim_dense() const {
    std::vector<float> dense((std::size_t)n_ * (std::size_t)n_, 0.0f);
    for (idx_t r = 0; r < (idx_t)n_; ++r)
      sim_.for_each_in_row(r, [&](idx_t c, float v) {
        dense[(std::size_t)r * (std::size_t)n_ + (std::size_t)c] = v;
      });
    return fnv_bytes(dense.data(), dense.size() * sizeof(float));
  }

  // The legacy's rankScoreSigmoid (RFE.cpp:195-201) and the rankNorm cell value,
  // exposed so the parity gate can check the narrowings directly.
  float rank_score_sigmoid(int x, int pk) const {
    if (x == 1) return 1;
    float sigmoidv = 1 / (1 + exp((-pa_) * (x - pk)));
    return 1 - sigmoidv;
  }
  float rank_norm_cell(int posqi, int posiq) const {
    // The legacy writes (pow(sig(posqi), 2) * sig(posiq)). Three things are
    // load-bearing and are reproduced literally:
    //   * pow() takes a DOUBLE here (the float arg is promoted), so the square
    //     is computed in double and the product narrows once, at the end;
    //   * rankScoreSigmoid narrows sigmoidv to float BEFORE the "1 -" subtract,
    //     which is float arithmetic (RFE.cpp:197-199);
    //   * pk is k/2 with INTEGER division (RFE.cpp:187).
    // pow(x,2) is NOT rewritten as x*x: under -ffast-math a compiler may do
    // exactly that, and that rewrite is a bit-level divergence to be measured
    // in Step 4, not assumed harmless.
    const int pk = k_ / 2;
    const float a = rank_score_sigmoid(posqi, pk);
    const float b = rank_score_sigmoid(posiq, pk);
    return (float)(pow((double)a, 2.0) * (double)b);
  }

 // --------------------------------------------------------------- F2 out --
  const M& emb() const { return emb_; }
  std::size_t nnz_emb() const { return emb_.nnz(); }
  // Number of emb cells whose value is EXACTLY 0.0f. Reported because its
  // non-zero-ness is what licenses NOT routing this phase through
  // multiply_and_prune; the parity gate asserts it matches the legacy's count.
  std::size_t emb_exact_zeros() const {
    std::size_t c = 0;
    for (idx_t r = 0; r < (idx_t)n_; ++r)
      emb_.for_each_in_row(r, [&](idx_t, float v) { if (v == 0.0f) ++c; });
    return c;
  }
  // The legacy's rankKScore (RFE.cpp:644-646), exposed for the parity gate.
  float rank_kscore_pub(int pos) const {
    return (float)(1 - (log((double)pos) / log((double)k_)));
  }
  std::uint64_t ck_emb() const { return ck_csr(emb_); }

 // --------------------------------------------------------------- F3 out --
  const M& remb() const { return remb_; }
  std::size_t nnz_remb() const { return remb_.nnz(); }
  // Legacy rows that exist but carry NO value. The legacy pre-creates a row for
  // every q in [0,n) (RFE.cpp:621-624), so remb has n keys; a bare transpose()
  // has only the rows that receive a value. Reported by the gate so the
  // difference is measured rather than assumed away.
  std::size_t remb_rows() const {
    std::size_t r = 0;
    for (idx_t q = 0; q < (idx_t)n_; ++q) if (remb_.row_nnz(q) > 0) ++r;
    return r;
  }
  std::uint64_t ck_remb() const { return ck_csr(remb_); }

  // --------------------------------------------------------------- F5 out --
  const std::vector<std::vector<int>>& sorted_emb() const { return sorted_emb_; }
  // Normalised effect estimation, one per row, AFTER division by gr_value_.
  const std::vector<float>& ee() const { return ee_; }
  // tag_sort's output: the row order, descending by UNNORMALISED ee. This is F6's
  // outer loop order, so its tie-break is part of the output (see f5 note).
  const std::vector<int>& sorted_ee() const { return sorted_ee_; }
  // The divisor, i.e. the maximum UNNORMALISED ee. Exposed so the gate can check
  // it directly instead of inferring it from the ratio of two ee values.
  float gr_value() const { return gr_value_; }
  std::size_t emb_topk_sum_zero_rows() const {
    std::size_t c = 0;
    for (int q = 0; q < n_; ++q) if (ee_[(std::size_t)q] == 0.0f) ++c;
    return c;
  }
  // Digests over the F5 state. ee is hashed as float bits; sorted_emb and
  // sorted_ee as int32, so a tie-break change in EITHER sort changes the digest
  // even when every ee VALUE is unchanged -- which is the whole risk here.
  std::uint64_t ck_ee() const {
    return fnv_bytes(ee_.data(), ee_.size() * sizeof(float));
  }
  std::uint64_t ck_sorted_emb() const {
    std::uint64_t h = 1469598103934665603ULL;
    for (int q = 0; q < n_; ++q) {
      const auto& row = sorted_emb_[(std::size_t)q];
      h = fnv_bytes(row.data(), row.size() * sizeof(int));
      h ^= (std::uint64_t)row.size();
      h *= 1099511628211ULL;
    }
    return h;
  }
  std::uint64_t ck_sorted_ee() const {
    return fnv_bytes(sorted_ee_.data(), sorted_ee_.size() * sizeof(int));
  }

 // --------------------------------------------------------------- F6 out --
  const M& gram() const { return gram_; }
  std::size_t nnz_gram() const { return gram_.nnz(); }
  // Gram cells whose value is EXACTLY 0.0f. These are the H8 population: real
  // candidates (incElems inserted them) that carry a zero value. They must
  // survive to F7, so the count is reported rather than assumed.
  std::size_t gram_exact_zeros() const {
    std::size_t c = 0;
    for (idx_t r = 0; r < (idx_t)n_; ++r)
      gram_.for_each_in_row(r, [&](idx_t, float v) { if (v == 0.0f) ++c; });
    return c;
  }
  const std::vector<std::vector<int>>& inc_elems_l() const { return inc_elems_l_; }
  std::uint64_t ck_gram() const { return ck_csr(gram_); }
  // Read-only view of the rank lists, for the end-to-end gate. rk_ is the same
  // object the legacy mutates in place, so this is the same aliasing the legacy
  // has -- the gate compares it against the reference's own vector.
  const std::vector<int>& rk_ref() const { return rk_; }

 private:
  // F1's cost attribution (Step 3 / Step 3c). Phases at the granularity the
  // legacy's structure actually has: index build (serial), the cell loop, the
  // CSR build (serial), and sort_ranked_lists. The in-loop split is differential.
  struct F1Profile {
    double t_total = 0.0;
    double t_index = 0.0;    // SERIAL: build_position_index
    double t_loop = 0.0;     // wall of the parallel region
    double t_cells_max = 0.0;// CRITICAL PATH inside the loop (max thread)
    double t_build = 0.0;    // SERIAL: build_from_partial_results
    double t_sortrows = 0.0; // sort_ranked_lists: stable_sort + self pin
    long long cells = 0;
    long long absent = 0;    // position_of returned the l+2 absent marker
    int threads = 1;
    bool enabled = false;
  };
  F1Profile f1prof_;
  bool f5sort_stable_ = false;

 public:
  void set_f1_profile(bool on) { f1prof_.enabled = on; }

  // Step 3 / Block 2: the F5 tie-break negative control. OFF by default, so no
  // parity gate can ever run the probed algorithm by accident.
  void set_f5_sort_probe(bool stable) { f5sort_stable_ = stable; }
  bool f5_sort_probe() const { return f5sort_stable_; }

  const F1Profile& f1_profile() const { return f1prof_; }

 public:
  // F6's cost attribution (Step 3 / Stage 3a).
  //
  // F6 is 66% of the real-data pipeline, and until this existed it was ONE
  // number. The question this answers is NOT "is F6 slow" -- it is "is F6 slow
  // because of STRICTNESS or because of IMPLEMENTATION", because the two have
  // different remedies: the first cannot be fixed (float addition order is
  // irreducible, see FINDINGS) and the second can.
  //
  // The split is deliberately at the boundaries the code already has:
  //   disc  -- pass 1, the union walk over sorted_emb
  //   sort  -- RowAcc::finalize_candidates (insertion sort + dedupe)
  //   acc   -- pass 2, the ordered accumulation (THE STRICT PART)
  //   emit  -- triplet emission
  // plus the two serial phases around the parallel region.
  // (read by the Stage 3a bench; the struct itself is public so the bench can
  // print the whole record without a dozen accessors)
  public:
  struct F6Profile {
    double t_inv = 0.0;         // serial: build_emb_inverted_by_sorted_ee
    double t_disc = 0.0;        // pass 1: candidate discovery
    double t_sort = 0.0;        // finalize_candidates
    double t_acc = 0.0;         // pass 2: ordered accumulation <- the strictness
    double t_emit = 0.0;        // triplet emission
    double t_build = 0.0;       // serial: build_from_partial_results
    double t_total = 0.0;       // whole f6_gram_and_inc_elems call
    long long cand_raw = 0;     // candidates WITH multiplicity (union size)
    long long cand_uniq = 0;    // distinct columns per row (Gram row length)
    long long inner_iters = 0;  // total pass-2 (q,j) visits
    long long inv_max = 0;      // largest |inv_of_col_[i]| -- load balance
    long long inv_min = 0;
    // Per-phase SUM over threads is CPU work; MAX over threads is the critical
    // path. With a static row split and unequal rows these diverge badly, and
    // conflating them is how one ends up with a 833% "percentage". Wall time is
    // governed by the MAX, so both are reported.
    double m_disc = 0, m_sort = 0, m_acc = 0, m_emit = 0;
    int threads = 1;
    bool enabled = false;
  };
  private:
  F6Profile f6prof_;

 public:
  // Test seam (Step 3 / Stage 3b, Camada 2). Runs the REAL finalize path on a
  // synthetic raw multiset and hands back the result, so the corner suite can
  // assert the two properties the O(k log k) rewrite depends on:
  //   (1) cols_out is DISTINCT and ASCENDING -> the builder's V4 fast path;
  //   (2) every vals_out bit-pattern is 0.0f -> no value is carried through
  //       pass 1. If a future refactor reintroduces a value-carrying candidate
  //       path, this is what notices, instead of the Gram quietly changing.
  // Same convention as rank_kscore_pub(): a thin wrapper over the real code,
  // never a re-implementation of it. int in / int out so the corner suite does
  // not have to name idx_t.
  static void probe_candidates(const std::vector<int>& raw,
                               std::vector<int>& cols_out,
                               std::vector<float>& vals_out) {
    RowAcc a;
    for (const int c : raw) a.add_candidate((idx_t)c);
    a.finalize_candidates();
    cols_out.clear();
    for (const idx_t c : a.cols) cols_out.push_back((int)c);
    vals_out = a.vals;
  }

  // Stage 3a toggle. OFF by default so the production path and every parity
  // gate run the identical code they ran before the instrumentation existed.
  void set_f6_profile(bool on) { f6prof_.enabled = on; }
  const F6Profile& f6_profile() const { return f6prof_; }

 public:
  // F6's per-row accumulator. Two phases on one buffer: `add_candidate` only
  // widens the column set (pass 1, the union), `add` accumulates a value into an
  // existing candidate (pass 2). Candidates are kept sorted by an explicit sort
  // before emission so the builder's V4 fast path sees strictly ascending columns
  // and has nothing to coalesce -- the same contract F2 relies on.
  struct RowAcc {
    std::vector<idx_t> cols;
    std::vector<float> vals;
    void clear() { cols.clear(); vals.clear(); }
    std::size_t size() const { return cols.size(); }
    // Called BETWEEN pass 1 and pass 2. Sorting alone is not enough: pass 1 takes
    // a union over q, so the same column is appended once per q that contributes
    // it. Without a unique, the duplicates would survive as extra cells and
    // pass 2's binary search would only ever find the FIRST of them, silently
    // dropping every later q's contribution to that column.
    //
    // WHY THERE IS NO VALUE TO SORT (Step 3 / Stage 3b, and the whole point):
    // pass 1 only ever calls add_candidate, which carried a literal 0.0f. The
    // previous version therefore shuffled `vals` alongside `cols` purely so it
    // could COALESCE duplicates by summing -- and 0.0f + 0.0f + ... + 0.0f is
    // exactly 0.0f in IEEE-754, for every order and every count. The value it
    // computed was a constant it already knew. So `vals` is not carried through
    // pass 1 at all, and the sort cannot desynchronise it from `cols`.
    //
    // That removes three failure modes at once, and they were not hypothetical:
    // the insertion sort below was O(k^2) on the RAW candidate count (~829/row
    // real, measured) and accounted for 77% of F6; a paired (col,val) sort is
    // the classic place to move one array and forget the other; and a refactor
    // that let pass 1 carry a real value would silently change the Gram.
    //
    // ORDER-SENSITIVITY: none. The output is the distinct ascending column set
    // with 0.0f values, which is the same set for any correct sort, because the
    // duplicates being collapsed carried no information. stable_sort is NOT
    // required and would be a different (slower) algorithm for the same answer.
    //
    // CONTRACT: the result is strictly ascending and unique, which is what lets
    // the builder's V4 fast path run with nothing to coalesce. vals is sized to
    // match exactly; pass 2's add() indexes it by binary search on cols.
    void finalize_candidates() {
      std::sort(cols.begin(), cols.end());
      cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
      vals.assign(cols.size(), 0.0f);
    }
    void add_candidate(idx_t c) {
      // Appended unsorted; finalize_candidates() sorts and dedupes. A fresh
      // column starts at 0.0f -- the legacy's zero-fill -- and that value is
      // now created in finalize_candidates(), where the distinct set is known,
      // rather than carried here where there is nothing to carry.
      cols.push_back(c);
    }
    idx_t col(std::size_t s) const { return cols[s]; }
    float val(std::size_t s) const { return vals[s]; }
    // Pass 2. Binary search for the candidate (always present: pass 2 iterates
    // the same sorted_emb lists pass 1 used), then add in CALL ORDER -- the legacy
    // order for that cell is the order of the q loop, which is sorted_ee.
    void add(idx_t c, float v) {
      std::size_t lo = 0, hi = cols.size();
      while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (cols[mid] < c) lo = mid + 1; else hi = mid;
      }
      if (lo < cols.size() && cols[lo] == c) vals[lo] += v;
    }
  };

  // FNV-1a over (row, col, value_bits) -- the canonical cell form.
  std::uint64_t ck_csr(const M& m) const {
    std::uint64_t h = 1469598103934665603ULL;   // the golden's literal, see below
    for (idx_t r = 0; r < (idx_t)n_; ++r) {
      m.for_each_in_row(r, [&](idx_t c, float v) {
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        std::uint64_t x = ((std::uint64_t)r << 32) ^ (std::uint64_t)c;
        h ^= x; h *= 1099511628211ULL;
        h ^= b; h *= 1099511628211ULL;
      });
    }
    return h;
  }

  // FNV-1a over the raw bytes, with the offset basis SPELLED THE SAME WAY the
  // golden's is. The golden (rfe_ref_verbatim.hpp) uses 1469598103934665603 --
  // 19 digits, which is NOT the canonical 20-digit FNV-1a basis
  // (14695981039346656037). That does not matter for correctness: a digest is
  // only ever compared against another digest produced by the SAME function, and
  // the pins in Data/ and in step0_check.sh were recorded with the golden's
  // literal. Writing the canonical basis here made ck_rk() disagree with the
  // Step 0 pin even though every value matched (the gate's position-by-position
  // check had already passed) -- a digest that is "more correct" but not
  // comparable is useless as a pin.
  static std::uint64_t fnv_bytes(const void* p, std::size_t nbytes) {
    const std::uint8_t* b = static_cast<const std::uint8_t*>(p);
    std::uint64_t h = 1469598103934665603ULL;   // must equal the golden's literal
    for (std::size_t i = 0; i < nbytes; ++i) {
      h ^= b[i];
      h *= 1099511628211ULL;
    }
    return h;
  }

// ---- position index (replaces the legacy's O(L) getPosition scan) ---------
  // getPosition(qImg, img) answers "where does img sit in row qImg's list", and
  // rankNorm calls it as getPosition(img_rl_i, q). The legacy scans the row; this
  // builds the INVERTED index of the rank lists once per phase and binary
  // searches it. Step 1's H9 measured 0 mismatches over every (q,i) pair, with the
  // absent case (legacy returns l+2) reproduced exactly.
  //
  // Layout: for each id c, the ascending list of (row, pos) with rk[row][pos]==c.
  // Built by a serial scan in ascending (row,pos), so each list is already
  // sorted by row, and a binary search on row finds the entry. The legacy returns
  // the FIRST match in the ROW it scans, which is the smallest pos for that
  // (row, id) pair -- satisfied because each (row,id) pair occurs at most once in
  // a permutation-shaped list; the parity gate covers the duplicate case.
  void build_position_index() {
    inv_rows_.assign((std::size_t)n_, {});
    inv_pos_.assign((std::size_t)n_, {});
    inv_n_.assign((std::size_t)n_, 0);
    for (std::size_t row = 0; row < (std::size_t)n_; ++row) {
      for (int i = 0; i < l_; ++i) {
        const int c = rk_[(std::size_t)l_ * row + (std::size_t)i];
        if (c < 0 || c >= (int)n_) continue;      // fail-fast guard, see below
        inv_rows_[(std::size_t)c].push_back((std::uint32_t)row);
        inv_pos_[(std::size_t)c].push_back((std::uint32_t)i);
        ++inv_n_[(std::size_t)c];
      }
    }
  }

  // Returns the legacy's getPosition(q_id, img) result, including l+2 absent.
  int position_of(idx_t q_id, idx_t img) const {
    const std::size_t c = (std::size_t)img;
    const std::vector<std::uint32_t>& rows = inv_rows_[c];
    // std::lower_bound over the (already ascending) row ids.
    auto it = std::lower_bound(rows.begin(), rows.end(), (std::uint32_t)q_id);
    if (it != rows.end() && *it == (std::uint32_t)q_id) {
      const std::size_t k = (std::size_t)(it - rows.begin());
      return (int)inv_pos_[c][k] + 1;
    }
    return l_ + 2;   // the legacy's absent marker, fed to rankScoreSigmoid
  }

  // ---- the F2 row accumulator ----------------------------------------------
  // Open-addressed, linear-probed, sized from the k + k^2 support bound so its
  // footprint is independent of n. It exists to make the ACCUMULATION ORDER the
  // legacy's (see f2_create_emb), and for two further reasons the engine's own
  // kernels cannot cover:
  //
  //   * it NEVER drops a value, not even an exact 0.0f. H2 measured 20.7% of
  //     emb keys as exactly zero and they are structural, so a |v| filter here
  //     would be a correctness bug, not a precision one;
  //   * add() is called in the legacy's call sequence, so each cell's partial
  //     sums are the legacy's. Emitting duplicate records and letting
  //     build_from_partial_results coalesce would instead inherit the order an
  //     UNSTABLE std::sort leaves behind (SparseMatrix.hpp:322-336).
  class EmbAcc {
   public:
    static constexpr idx_t EMPTY = (idx_t)-1;

    // capacity: next power of two >= 2*bound, so the load factor stays <= 50%
    // and linear probing cannot degenerate (the same reasoning the engine's
    // HashRowAccumulator documents at SparseMatrix.hpp:453-461).
    void init(std::size_t bound) {
      std::size_t cap = 8;
      while (cap < bound * 2 + 2) cap <<= 1;
      slot_col_.assign(cap, EMPTY);
      slot_val_.assign(cap, 0.0f);
      mask_ = cap - 1;
      used_.reserve(bound + 2);
      ord_.reserve(bound + 2);
    }

    void begin() {
      // Clear only the slots this row actually touched -- O(k^2), not O(cap).
      for (std::size_t s : used_) slot_col_[s] = EMPTY;
      used_.clear();
      ord_.clear();
    }

    inline void add(idx_t col, float v) {
      std::size_t h = mix(col) & mask_;
      while (slot_col_[h] != EMPTY && slot_col_[h] != col) h = (h + 1) & mask_;
      if (slot_col_[h] == EMPTY) {
        // First sight of this column. INSERT unconditionally, even when v is
        // exactly 0.0f -- that is the legacy's incEmb else-branch (RFE.cpp:692)
        // and it is what H2 measured as load-bearing.
        slot_col_[h] = col;
        slot_val_[h] = v;
        used_.push_back(h);
        ord_.push_back(h);
      } else {
        slot_val_[h] += v;   // float += float, in the legacy's call order
      }
    }

    // Ascending column order, so the emitted row is strictly increasing and the
    // builder's V4 fast path applies with nothing to coalesce.
    void sort_columns() {
      std::sort(ord_.begin(), ord_.end(),
                [this](std::size_t a, std::size_t b) { return slot_col_[a] < slot_col_[b]; });
    }

    std::size_t size() const { return ord_.size(); }
    idx_t col(std::size_t i) const { return slot_col_[ord_[i]]; }
    float val(std::size_t i) const { return slot_val_[ord_[i]]; }

   private:
    static std::size_t mix(idx_t x) {
      std::uint64_t h = x;
      h ^= h >> 16; h *= 0x85ebca6bULL;
      h ^= h >> 13; h *= 0xc2b2ae35ULL;
      h ^= h >> 16;
      return (std::size_t)h;
    }

    std::vector<idx_t> slot_col_;
    std::vector<float> slot_val_;
    std::vector<std::size_t> used_;   // slots touched this row
    std::vector<std::size_t> ord_;    // slots in emitted (ascending col) order
    std::size_t mask_ = 0;
  };

  // The legacy's sort (RFE.cpp:724-767), reproduced.
  //
  // DIRECTION -- and this is the bug the first version of this file had, found
  // by the parity gate, not by reading. The legacy's insertion loop is
  //
  //     while (i >= 0 && a[i] < keyA) { a[i+1] = a[i]; ... i--; }
  //     a[i+1] = keyA;
  //
  // which shifts every element STRICTLY SMALLER than the key one slot right and
  // drops the key where the run of smaller elements began. So SMALLER values end
  // up LATER: the row comes out DESCENDING, most similar first. The port's
  // comparator was `a.first < b.first` (ascending), which produced the exact
  // mirror: the core's row 0 was 190(2.07e-05) 62(2.52e-05) ... where the
  // legacy's is 0(1) 95(0.0229) 168(0.0156) ... -- the same multiset, reversed.
  //
  // STABILITY, once the direction is right, still matters: the loop shifts only
  // on strict `<`, so it is a STABLE sort, and equal values keep their original
  // list order. std::stable_sort with the same key and the same input order
  // therefore yields the same permutation. Measured: 0 exact-value ties on the
  // pinned row, and the H6 tie-invariance result covers the general case for
  // ee -- but the self-pin below is NOT a tie question and must be kept.
  //
  // The comparator is written as `>` so the direction is visible at the call
  // site; `std::stable_sort` then produces most-similar-first.
  void sort_ranked_lists() {
    // Per-row violation flags. Distinct indices are written from distinct
    // iterations, so this needs no synchronisation; the throws happen AFTER the
    // region because throwing from inside an OpenMP parallel region is forbidden
    // by the standard and libgomp turns it into std::terminate.
    std::vector<char> bad_sim((std::size_t)n_, 0);
    std::vector<char> missing_self((std::size_t)n_, 0);
    #pragma omp parallel
    {
      std::vector<std::pair<float, int>> local;
      #pragma omp for schedule(static)
      for (long long q = 0; q < (long long)n_; ++q) {
        local.clear();
        local.reserve((std::size_t)l_);
        for (int i = 0; i < l_; ++i) {
          const int img = rk_[(std::size_t)l_ * q + (std::size_t)i];
          // Absent cells read the legacy's zero-fill (initSparseMatrix), which is
          // exactly what get() returns for an absent cell -- same value.
          const float v = sim_.get((idx_t)q, (idx_t)img);
          if (!std::isfinite(v)) {
            // A NaN makes the legacy's `a[i] < keyA` false for every a[i], at
            // which point its order is not reproduced by any sort. Refuse rather
            // than silently differ -- but RECORD, do not throw: see the note at
            // the self-pin below about throwing inside an OpenMP region.
            bad_sim[(std::size_t)q] = 1;
            break;
          }
          local.emplace_back(v, img);
        }
        std::stable_sort(local.begin(), local.end(),
                         [](const std::pair<float, int>& a,
                            const std::pair<float, int>& b) { return a.first > b.first; });
        for (int i = 0; i < l_; ++i)
          rk_[(std::size_t)l_ * q + (std::size_t)i] = local[(std::size_t)i].second;

        // Self pinned to position 0, unconditionally (RFE.cpp:754-766). This is
        // NOT a tie question: it is a separate swap applied AFTER the sort. The
        // legacy's scan reads element i before testing i < l_, so a row without
        // its own id reads out of bounds; the contract is that self is present.
        //
        // THE VIOLATION IS RECORDED, NOT THROWN, HERE. Throwing from inside an
        // OpenMP parallel region is forbidden by the C++ standard, and libgomp
        // responds by calling std::terminate -- so a `throw` here would abort the
        // process instead of failing closed. The corner suite caught this: the
        // guard fired but the exception never reached the caller. The flag is
        // written (distinct indices, so no data race) and the throw happens after
        // the region, where it propagates normally.
        int self_pos = -1;
        for (int i = 0; i < l_; ++i) {
          if (rk_[(std::size_t)l_ * q + (std::size_t)i] == (int)q) { self_pos = i; break; }
        }
        if (self_pos < 0) {
          missing_self[(std::size_t)q] = 1;
          continue;
        }
        if (self_pos > 0) {
          std::swap(rk_[(std::size_t)l_ * q], rk_[(std::size_t)l_ * q + (std::size_t)self_pos]);
        }
      }
    }
    // Outside the parallel region, where a throw actually propagates.
    for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
      if (bad_sim[q])
        throw std::runtime_error(
            "RFE: non-finite similarity in row " + std::to_string(q) +
            "; the legacy's order is undefined for NaN and would diverge from "
            "std::stable_sort");
    }
    for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
      if (missing_self[q])
        throw std::runtime_error(
            "RFE: rank list row " + std::to_string(q) +
            " does not contain its own id; the legacy reads out of bounds at this "
            "point (RFE.cpp:754-756)");
    }
  }

  std::vector<int>& rk_;
  std::size_t n_;
  int l_, k_, t_;
  double pa_;
  M sim_;
  M emb_;
  M remb_;
  // F5 state. sorted_emb_[q] is the KEY order of emb[q] after sortEmb's
  // descending std::sort (kept, not just its first k, because that order IS the
  // legacy's output). ee_ holds the NORMALISED values; sorted_ee_ is the row
  // order by UNNORMALISED ee and becomes F6's outer loop order.
  std::vector<std::vector<int>> sorted_emb_;
  std::vector<float> ee_;
  std::vector<int> sorted_ee_;
  float gr_value_ = 0.0f;
  // MX(q,q) = pow(2,30) (RFE.cpp:605). Applied at READ time by F7b rather than
  // written into gram_, so gram_ stays equal to the ACCUMULATED Gram that F6
  // gated. Written the way the legacy writes it: pow() with int arguments,
  // narrowed once to float.
  const float diag_pin_ = (float)pow(2, 30);
  // F6 state. gram_ carries the Gram AND doubles as incElems (same support by
  // construction). inv_of_col_[i] = ascending sorted_ee positions t with
  // i in sorted_emb[sorted_ee[t]] -- the row-independent q order F6 iterates.
  M gram_;
  std::vector<std::vector<std::uint32_t>> inv_of_col_;
  std::vector<std::vector<int>> inc_elems_l_;
  std::vector<std::vector<std::uint32_t>> inv_rows_;
  std::vector<std::vector<std::uint32_t>> inv_pos_;
  std::vector<std::size_t> inv_n_;

  // F8 state (Etapa 3-6: PERFORM_CCS)
  // th_cc_ mirrors the legacy's double member (repo-root RFE.hpp:140).
  double th_cc_ = 0.0;
  // H27 (Section 1): the CC flag is stored so run() can refuse it (Etapa 9 will
  // consume it for the real post-f7b CC sequence and drop the throw).
  bool performCCs_ = false;
  // F8a: edges_ccs_ is the FULL sorted edge vector (never truncated -- the
  // reference's processCCs iterates by value and breaks, it does not resize).
  // f8c_edge_prefix() derives the prefix count processCCs consumes and stores
  // it in f8_prefix_m_; f8d_process_ccs() unions only that prefix.
  std::vector<std::pair<float, std::pair<int, int>>> edges_ccs_;
  long long f8_prefix_m_ = -1;  // prefix count from f8c; -1 = f8c not run yet
  bool f8_edges_parallel_ = true;  // Etapa 3: variant B measured winner
  // F8d: icc label per object (from DSU with legacy label semantics).
  std::vector<int> f8_cc_label_;
  // Etapa 4/6b: f8d variant selector. true = DSU (measured 116x faster on ALOI,
  // default); false = literal map<int,set> + set_union, kept as a differential
  // oracle that mirrors the reference's unionCCs/sameCC exactly.
  bool f8_components_dsu_ = true;
  // F8e: members/rows indexed BY ORIGINAL LABEL (size n_), empty for singletons.
  // ccemb_rows_[r] = ascending-key vector of (key, val) for CC label r.
  // sorted_ccemb_[r] = descending-value keys for CC label r.
  std::vector<std::vector<int>> f8_cc_members_;       // size n_, by original label
  std::vector<std::vector<std::pair<int, float>>> ccemb_rows_;  // size n_, by original label
  std::vector<std::vector<int>> sorted_ccemb_;        // size n_, by original label
  bool f8_ccemb_hash_ = false;      // accum variant (A dense / B hash)
  bool f8_ccemb_fastsort_ = false;  // sort variant (A full / B fast top-m)
  long long f8e_fallbacks_ = 0;
  long long self_off0_ = 0;        // H19: rows with self off index 0 at entry
  // F8f: embycc = simEmb(emb[q], ccemb[r]) for all q and all non-singleton CC labels r.
  // Dense n x nCC (A) vs CSR Gustavson (B) -- both MUST produce bit-identical
  // simEmb(embycc[k1], embycc[k2]) for the downstream F8g.
  // inv_ccemb_[key] = vector of (dense_cc_index, val) for Gustavson.
  std::vector<std::vector<std::pair<int, float>>> inv_ccemb_;  // key -> (dense_cc_idx, val)
  std::vector<int> label_to_idx_;      // original CC label -> 0..ncc-1 column index (-1 for singletons)
  struct EmbCCMatrix {
    std::vector<int> row_ptr;
    std::vector<int> col_idx;   // dense CC indices (0..ncc-1, sorted ascending)
    std::vector<float> values;  // non-negative, finite
    int nrows = 0, ncols = 0;
    float get(int q, int r) const {
      if (q < 0 || q >= nrows) return 0.0f;
      int lo = row_ptr[(std::size_t)q];
      int hi = row_ptr[(std::size_t)q + 1];
      auto it = std::lower_bound(col_idx.begin() + lo, col_idx.begin() + hi, r);
      if (it == col_idx.begin() + hi || *it != r) return 0.0f;
      return values[(std::size_t)(it - col_idx.begin())];
    }
  };
  EmbCCMatrix emb_by_cc_sparse_;
  std::vector<float> emb_by_cc_dense_;  // dense n x n matrix for Variant A
  bool f8_embycc_sparse_ = true;  // default: B (CSR Gustavson)
  // F8g state (Etapa 7): the reRankingByCCs cell scratch. f8g_matrix_ is the
  // dense n x n similarity matrix the cells are written into (the legacy's MX,
  // a zero-filled std::vector<float>; there is NO diagonal pin here, unlike the
  // CPRR Gram -- untouched cells stay 0.0f and the tail sorts them as ordinary
  // zeros). Written per (k1,k2) with a plain float assignment, so when several
  // CCs target the same cell the LAST CC in ascending-label order wins.
  std::vector<float> f8g_matrix_;
  // inc_elems_[k1] = set of k2 ever written by a cell (mirrors the legacy's
  // incElems_, the union over CCs of their sorted_ccemb top-k2 members);
  // inc_elems_l_[q] = the ids in q's POST-f7b rank list (the legacy's
  // incElemsL_). F8h consumes set_difference(inc_elems_, inc_elems_l_).
  std::vector<std::set<int>> f8g_inc_elems_;
  std::vector<std::set<int>> f8g_inc_elems_l_;
  long long f8g_pos_absent_ = 0;  // diagnostic: getPosition hits that returned l+2

  // F8g accessors (Etapa 7) for the CC parity gate. f8g_cell reads the dense
  // scratch so the gate can diff it cell-by-cell against the reference's MX.
  const std::vector<float>& f8g_matrix() const { return f8g_matrix_; }
  float f8g_cell(int k1, int k2) const {
    if (k1 < 0 || k1 >= (int)n_ || k2 < 0 || k2 >= (int)n_) return 0.0f;
    return f8g_matrix_[(std::size_t)k1 * (std::size_t)n_ + (std::size_t)k2];
  }
  const std::vector<std::set<int>>& f8g_inc_elems() const { return f8g_inc_elems_; }
  const std::vector<std::set<int>>& f8g_inc_elems_l() const { return f8g_inc_elems_l_; }
  long long f8g_pos_absent() const { return f8g_pos_absent_; }

  // F8 accessors for CC parity gate
  long long self_off0() const { return self_off0_; }  // H19: rows with self off index 0 at entry
  const std::vector<std::pair<float, std::pair<int, int>>>& edges_ccs() const { return edges_ccs_; }
  double th_cc() const { return th_cc_; }
  void set_th_cc(double v) { th_cc_ = v; }
  // H30 (Etapa 6b item 3): EXACT closed form of the legacy's processCCs prefix.
  // The legacy loop (RFE.cpp:348-363 / ref:849-862) is:
  //   int i = 0; for (auto t : edgesCCs_) { if (i >= E * th_cc) break; ... i++; }
  // so it processes exactly ceil(E * th_cc) edges (E = size). NO epsilon: a
  // bound of 34.00000000000001 must consume 35 edges (ceil == 35), and the old
  // `-1e-12` wrongly returned 34. Public+static so the gate can test it in
  // isolation against a replay of the legacy loop for arbitrary (E, th).
  static long long f8_prefix_closed_form(long long E, double th) {
    if (E <= 0) return 0;
    if (th <= 0.0) return 0;
    double prod = (double)E * th;
    long long m = (long long)std::ceil(prod);
    if (m < 0) m = 0;
    if (m > E) m = E;
    return m;
  }
  long long f8_prefix() const { return f8_prefix_m_; }
  void set_f8_edges_parallel(bool on) { f8_edges_parallel_ = on; }
  bool f8_edges_parallel() const { return f8_edges_parallel_; }
  const std::vector<std::vector<std::pair<int, float>>>& ccemb_rows() const { return ccemb_rows_; }
  const std::vector<std::vector<int>>& sorted_ccemb() const { return sorted_ccemb_; }
  const std::vector<int>& f8_cc_label() const { return f8_cc_label_; }
  void set_f8_embycc_sparse(bool on) { f8_embycc_sparse_ = on; }
  bool f8_embycc_sparse() const { return f8_embycc_sparse_; }
  // CC emb variants (Etapa 5)
  void set_f8_ccemb_hash(bool on) { f8_ccemb_hash_ = on; }
  bool f8_ccemb_hash() const { return f8_ccemb_hash_; }
  void set_f8_ccemb_fastsort(bool on) { f8_ccemb_fastsort_ = on; }
  bool f8_ccemb_fastsort() const { return f8_ccemb_fastsort_; }
  long long f8e_fastsort_fallbacks() const { return f8e_fallbacks_; }
  // F8d variant (Etapa 4/6b): DSU (default, measured winner) vs literal
  // map<int,set>+set_union (differential oracle mirroring the reference).
  void set_f8_components_dsu(bool on) { f8_components_dsu_ = on; }
  bool f8_components_dsu() const { return f8_components_dsu_; }
  float embycc_get(int q, int r) const;
  float sim_embycc(int k1, int k2) const;

  // F8 phase methods
  void f8a_create_edges_ccs();
  double f8b_init_th_cc();
  long long f8c_edge_prefix();
  void f8d_process_ccs();
  void f8e_compute_ccs_emb();
  void f8f_compute_embs_by_ccs();
  // F8g (Etapa 7): the CELL computation of reRankingByCCs (ref:981-1021), up to
  // and including the incElems_/incElemsL_ bookkeeping but NOT the tail sort
  // (kernel_sort_ranked_lists_extra) -- that is F8h. Populates the dense
  // f8g_matrix_ (zero-fill; last-plain-write wins per cell, no diagonal pin) and
  // inc_elems_. Requires f8f (reads emb_by_cc_*) and a position index (builds one
  // if absent, since getPosition reads the rank lists POST-f7b).
  void f8g_rerank_cells();

  // F8h (Etapa 8): the TAIL of reRankingByCCs (ref:1012-1020). Mirrors
  // f7b_rerank_cprr_tail but uses f8g_matrix_ (no diagonal pin, unwritten = 0)
  // and f8g_inc_elems_/f8g_inc_elems_l_ built by f8g.
  void f8h_rerank_tail();
};

// F8 implementations

inline float RFECore::embycc_get(int q, int r) const {
  if (f8_embycc_sparse_) {
    // Convert original CC label to dense column index
    if (r < 0 || r >= (int)label_to_idx_.size()) return 0.0f;
    int ci = label_to_idx_[(std::size_t)r];
    if (ci < 0) return 0.0f;
    return emb_by_cc_sparse_.get(q, ci);
  } else {
    // Dense variant: emb_by_cc_dense_ is n x n, indexed by [q * n + r]
    if (q < 0 || q >= (int)n_ || r < 0 || r >= (int)n_) return 0.0f;
    return emb_by_cc_dense_[(std::size_t)q * (std::size_t)n_ + (std::size_t)r];
  }
}

inline float RFECore::sim_embycc(int k1, int k2) const {
  // Match legacy's simEmb order: merge-join in ascending ORIGINAL CC LABEL order.
  // The reference merge-joins two std::map (sorted by key = original CC label).
  // The core's dense ci is assigned in ascending original-label order (f8e sorts
  // label_list before numbering) and f8f writes each CSR row's columns ASCENDING,
  // so a two-pointer merge over the two rows visits nonzero terms in the exact
  // same order -- bit-exact -- in O(nnz1+nnz2) instead of the former O(n) scan.
  float sum = 0.0f;
  if (f8_embycc_sparse_) {
    if (k1 < 0 || k1 >= (int)n_ || k2 < 0 || k2 >= (int)n_) return 0.0f;
    const auto& sp = emb_by_cc_sparse_;
    int i = sp.row_ptr[(std::size_t)k1], hi1 = sp.row_ptr[(std::size_t)k1 + 1];
    int j = sp.row_ptr[(std::size_t)k2], hi2 = sp.row_ptr[(std::size_t)k2 + 1];
    while (i < hi1 && j < hi2) {
      const int c1 = sp.col_idx[(std::size_t)i];
      const int c2 = sp.col_idx[(std::size_t)j];
      if (c1 < c2) {
        ++i;
      } else if (c2 < c1) {
        ++j;
      } else {
        const float v1 = sp.values[(std::size_t)i];
        const float v2 = sp.values[(std::size_t)j];
        // Sum over the COLUMN-INTERSECTION (== the two maps' key-intersection),
        // matching simEmb (ref:481-489) exactly. A stored 0.0f is a STRUCTURAL
        // zero (an embedding key whose value collapsed to zero, kept as a key per
        // ref:460-463), so it MUST be visited and contribute v*0.0f -- skipping it
        // would change the SUPPORT, which is the exact defect the reference comment
        // warns about (F8g's cell diff caught the old `!= 0.0f` guard here).
        sum += v1 * v2;
        ++i; ++j;
      }
    }
  } else {
    // Dense variant: use the exact dense embycc matrix computed by legacy's algorithm
    // sum over all CC labels r of embycc[k1][r] * embycc[k2][r]
    if (k1 < 0 || k1 >= (int)n_ || k2 < 0 || k2 >= (int)n_) return 0.0f;
    for (int r = 0; r < (int)n_; ++r) {
      float v1 = emb_by_cc_dense_[(std::size_t)k1 * (std::size_t)n_ + (std::size_t)r];
      float v2 = emb_by_cc_dense_[(std::size_t)k2 * (std::size_t)n_ + (std::size_t)r];
      // Column-intersection == the maps' key-intersection (see the sparse branch):
      // a 0.0f cell is a structural zero and must still contribute v*0.0f, or the
      // SUPPORT diverges from simEmb (ref:481-489). Every column r holds a stored
      // key, so all r are "present"; the product is simply accumulated.
      sum += v1 * v2;
    }
  }
  return sum;
}

inline void RFECore::f8a_create_edges_ccs() {
  edges_ccs_.clear();
  edges_ccs_.reserve((std::size_t)n_ * (std::size_t)k_);
  std::vector<char> bad_conf((std::size_t)n_, 0);
  const int nt = omp_get_max_threads();
  const std::size_t chunk = ((std::size_t)n_ + (std::size_t)nt - 1) / (std::size_t)nt;

  auto emit = [&](std::size_t r0, std::size_t r1,
                  std::vector<std::pair<float, std::pair<int, int>>>& out) {
    std::vector<std::pair<idx_t, float>> ra, rb;
    for (std::size_t q = r0; q < r1; ++q) {
      ra.clear();
      emb_.for_each_in_row((idx_t)q,
                           [&](idx_t c, float v) { ra.emplace_back(c, v); });
      for (int i = 0; i < k_; ++i) {
        const int img_nn = rk_[(std::size_t)l_ * q + (std::size_t)i];
        if ((int)q == img_nn) continue;
        rb.clear();
        emb_.for_each_in_row((idx_t)img_nn,
                             [&](idx_t c, float v) { rb.emplace_back(c, v); });
        const float sim = merge_sim(ra, rb);
        const float conf = (sim * ee_[(std::size_t)q]) * ee_[(std::size_t)img_nn];
        if (!std::isfinite(conf)) bad_conf[(std::size_t)q] = 1;
        out.push_back({conf, {(int)q, img_nn}});
      }
    }
  };

  if (!f8_edges_parallel_) {
    emit(0, (std::size_t)n_, edges_ccs_);
  } else {
    std::vector<std::vector<std::pair<float, std::pair<int, int>>>> bufs((std::size_t)nt);
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int c = 0; c < nt; ++c) {
      const std::size_t r0 = std::min((std::size_t)n_, (std::size_t)c * chunk);
      const std::size_t r1 = std::min((std::size_t)n_, (std::size_t)(c + 1) * chunk);
      emit(r0, r1, bufs[(std::size_t)c]);
    }
    for (int c = 0; c < nt; ++c)
      edges_ccs_.insert(edges_ccs_.end(), bufs[(std::size_t)c].begin(), bufs[(std::size_t)c].end());
  }

  // Legacy sorts descending lexicographically: conf desc, then q desc, then nn desc
  std::sort(edges_ccs_.rbegin(), edges_ccs_.rend());

  for (std::size_t q = 0; q < (std::size_t)n_; ++q)
    if (bad_conf[q])
      throw std::runtime_error("RFE: non-finite conf in row " + std::to_string(q) + " -- legacy order undefined");
}

inline double RFECore::f8b_init_th_cc() {
  // Mirror legacy's initEdgesCCs automatic rule (RFE.cpp:348-355):
  // th_cc = (avg of ee over sorted_ee order) / 2
  // Legacy uses FLOAT accumulation for bit-exactness.
  // H23 (2026-10-09): the legacy GUARDS the auto rule with `if (th_cc == 0)` --
  // a fixed PARAM_RFE_TH_CC input (0.05/0.5/1.0/2.0) must be PRESERVED, not
  // overwritten. Without the guard every th>0 case failed F8b and cascaded
  // through F8c/F8d/F8e/F8f (different prefix -> different CCs -> ccemb ->
  // embycc).
  if (sorted_ee_.empty() || ee_.empty())
    throw std::runtime_error("RFE: f8b_init_th_cc called before ee/sorted_ee ready");
  if (th_cc_ == 0) {
    float avg = 0.0f;
    for (int q : sorted_ee_) {
      avg += ee_[(std::size_t)q];
    }
    avg /= (float)n_;
    th_cc_ = (double)(avg / 2.0f);  // Store as double but computed as float
  }
  return th_cc_;
}

inline long long RFECore::f8c_edge_prefix() {
  // H30 (Etapa 6b item 3): exact closed form, NO epsilon, NO truncation. The
  // reference's processCCs iterates by value and breaks on the bound -- it never
  // resizes edgesCCs_ -- so we store the prefix COUNT and leave the full vector
  // intact. f8d unions only the first f8_prefix_m_ edges. (The old code did
  // `ceil(prod - 1e-12)` then `resize(m)`: the epsilon under-counted a bound of
  // 34.00000000000001 to 34 instead of 35, and the resize truncated the accessor.)
  const long long E = (long long)edges_ccs_.size();
  const long long m = f8_prefix_closed_form(E, th_cc_);
  f8_prefix_m_ = m;
  return m;
}

inline void RFECore::f8d_process_ccs() {
  // H30 (Etapa 6b item 3): f8c stores the prefix COUNT (it no longer truncates
  // edges_ccs_). f8d must union exactly the first f8_prefix_m_ edges. If f8c has
  // not run, refuse rather than silently unioning the FULL vector.
  if (f8_prefix_m_ < 0) {
    throw std::runtime_error(
        "RFE: f8d_process_ccs called before f8c_edge_prefix (no prefix count)");
  }
  const long long m = std::min<long long>(f8_prefix_m_, (long long)edges_ccs_.size());

  // Variant A (literal, Etapa 6b item 4): map<label,set<member>> + set_union,
  // a faithful mirror of the reference's unionCCs/sameCC (ref:891-908). Kept as
  // a DIFFERENTIAL ORACLE: the DSU (variant B) must produce identical labels.
  // The reference's unionCCs survives r1 (= icc[o1]) and merges r2 into it via
  // set_union, then repoints every member of r2 to r1 and erases rcc[r2].
  if (!f8_components_dsu_) {
    std::map<int, std::set<int>> rcc;
    std::vector<int> icc((std::size_t)n_);
    for (int q = 0; q < (int)n_; ++q) {
      icc[(std::size_t)q] = q;
      rcc[q] = std::set<int>{q};
    }
    for (long long i = 0; i < m; ++i) {
      auto& e = edges_ccs_[(std::size_t)i];
      const int o1 = e.second.first, o2 = e.second.second;
      const int r1 = icc[(std::size_t)o1], r2 = icc[(std::size_t)o2];
      if (r1 == r2) continue;  // sameCC
      std::set<int> merged;
      std::set_union(rcc[r2].cbegin(), rcc[r2].cend(),
                     rcc[r1].cbegin(), rcc[r1].cend(),
                     std::inserter(merged, merged.cend()));
      rcc[r1] = std::move(merged);
      for (int o : rcc[r2]) icc[(std::size_t)o] = r1;
      rcc.erase(r2);
    }
    f8_cc_label_ = std::move(icc);
    return;
  }

  f8_cc_label_.assign((std::size_t)n_, 0);
  for (std::size_t i = 0; i < (std::size_t)n_; ++i) f8_cc_label_[(std::size_t)i] = (int)i;

  auto find = [&](int x) {
    int r = x;
    while (f8_cc_label_[(std::size_t)r] != r) r = f8_cc_label_[(std::size_t)r];
    int cur = x;
    while (f8_cc_label_[(std::size_t)cur] != cur) {
      int nxt = f8_cc_label_[(std::size_t)cur];
      f8_cc_label_[(std::size_t)cur] = r;
      cur = nxt;
    }
    return r;
  };

  for (long long i = 0; i < m; ++i) {
    auto& e = edges_ccs_[(std::size_t)i];
    int a = e.second.first;
    int b = e.second.second;
    int ra = find(a);
    int rb = find(b);
    if (ra != rb) {
      f8_cc_label_[(std::size_t)rb] = ra;
    }
  }
  for (std::size_t i = 0; i < (std::size_t)n_; ++i) {
    f8_cc_label_[(std::size_t)i] = find((int)i);
  }
}

inline void RFECore::f8e_compute_ccs_emb() {
  std::vector<int> label_list;
  label_list.reserve((std::size_t)n_);
  std::vector<char> is_root((std::size_t)n_, 0);
  for (std::size_t i = 0; i < (std::size_t)n_; ++i) {
    int lbl = f8_cc_label_[(std::size_t)i];
    if (!is_root[(std::size_t)lbl]) {
      is_root[(std::size_t)lbl] = 1;
      label_list.push_back(lbl);
    }
  }
  // Sort label_list so dense indices match original label order.
  // This makes CSR column order match legacy's std::map key order.
  std::sort(label_list.begin(), label_list.end());
  const int ncc = (int)label_list.size();
  label_to_idx_.assign((std::size_t)n_, -1);
  for (int i = 0; i < ncc; ++i)
    label_to_idx_[(std::size_t)label_list[i]] = i;

  // f8_cc_members_, ccemb_rows_, sorted_ccemb_ are now size n_ (by original label)
  f8_cc_members_.assign((std::size_t)n_, {});
  ccemb_rows_.assign((std::size_t)n_, {});
  sorted_ccemb_.assign((std::size_t)n_, {});

  // Populate f8_cc_members_ by original label
  for (std::size_t i = 0; i < (std::size_t)n_; ++i) {
    int lbl = f8_cc_label_[(std::size_t)i];
    if (lbl >= 0 && lbl < (int)n_) {
      f8_cc_members_[(std::size_t)lbl].push_back((int)i);
    }
  }

  const bool use_hash = f8_ccemb_hash_;
  const bool fastsort = f8_ccemb_fastsort_;

  // Work list: only roots with >1 member produce a ccemb row (the reference's
  // computeCCsEmb skips d.size()<=1). Parallelised ACROSS CCs: each CC writes
  // only ccemb_rows_[lbl]/sorted_ccemb_[lbl] (disjoint by lbl) and accumulates
  // serially in ascending-member/ascending-key order, so the float sum and the
  // unstable-sort tie order are bit-identical regardless of thread count (the
  // sort input order + comparator match the reference's sortCCEmb exactly).
  std::vector<int> work;
  work.reserve(label_list.size());
  for (int lbl : label_list) {
    if (label_to_idx_[(std::size_t)lbl] < 0) continue;
    if (f8_cc_members_[(std::size_t)lbl].size() > 1) work.push_back(lbl);
  }

  long long fallbacks = 0;
  #pragma omp parallel reduction(+:fallbacks)
  {
    // Per-THREAD buffers scoped to this region (NOT thread_local -- that was the
    // H21 bug). The STAMP accumulator reuses dense[] across the CCs this thread
    // visits, removing the O(n) alloc+zero of dense/visited paid PER CC before.
    std::vector<float> dense((std::size_t)n_, 0.0f);
    std::vector<int> stamp((std::size_t)n_, 0);
    std::vector<int> touched;
    touched.reserve(1024);
    std::vector<std::pair<int, float>> src, dstp;
    int cur = 0;

    #pragma omp for schedule(dynamic)
    for (int wi = 0; wi < (int)work.size(); ++wi) {
      const int lbl = work[(std::size_t)wi];
      const auto& members = f8_cc_members_[(std::size_t)lbl];
      ++cur;
      touched.clear();

      if (!use_hash) {
        // Dense stamp accumulation. First touch of a key in THIS CC zeroes it so
        // the first add is 0.0f + v (exactly the reference's map-insert semantics);
        // later adds run in ascending-member/ascending-key order (== reference).
        for (int obj : members) {
          emb_.for_each_in_row((idx_t)obj, [&](idx_t c, float v) {
            if (stamp[(std::size_t)c] != cur) {
              stamp[(std::size_t)c] = cur;
              dense[(std::size_t)c] = 0.0f;
              touched.push_back((int)c);
            }
            dense[(std::size_t)c] += v;
          });
        }
        std::sort(touched.begin(), touched.end());  // ascending key == std::map order
        src.clear();
        src.reserve(touched.size());
        for (int k : touched) src.emplace_back(k, dense[(std::size_t)k]);
      } else {
        std::unordered_map<int, float> acc;
        acc.reserve(members.size() * 8);
        for (int obj : members) {
          emb_.for_each_in_row((idx_t)obj, [&](idx_t c, float v) { acc[(int)c] += v; });
        }
        src.clear();
        src.reserve(acc.size());
        for (auto& kv : acc) src.emplace_back(kv.first, kv.second);
        std::sort(src.begin(), src.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
      }

      ccemb_rows_[(std::size_t)lbl] = src;  // ascending key, float bits == reference

      // sorted_ccemb: sort a COPY of src (ascending) by value DESC with the strict
      // `>` comparator -- byte-for-byte the reference's sortCCEmb(CmpDesc); the
      // unstable tie order is identical because input order and comparator match.
      dstp = src;
      const std::size_t N = dstp.size();
      const auto cmp = [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
        return a.second > b.second;
      };
      if (fastsort && N > 32) {
        const std::size_t m = std::min((std::size_t)k_, N);
        std::partial_sort(dstp.begin(), dstp.begin() + (std::ptrdiff_t)m, dstp.end(), cmp);
        // Real partial_sort with a tie guard: the prefix [0,m) matches the full
        // sort (and thus the reference) only when the top-m values are distinct AND
        // strictly greater than every tail value. On any tie, fall back to the full
        // sort so the prefix stays bit-identical (ties are ~0.3% of CCs).
        bool unsafe = false;
        for (std::size_t i = 0; i + 1 < m; ++i)
          if (!(dstp[i].second > dstp[i + 1].second)) { unsafe = true; break; }
        if (!unsafe && m < N) {
          float mx = dstp[m].second;
          for (std::size_t j = m + 1; j < N; ++j)
            if (dstp[j].second > mx) mx = dstp[j].second;
          if (!(dstp[m - 1].second > mx)) unsafe = true;
        }
        if (unsafe) {
          ++fallbacks;
          // Restore the PRISTINE ascending-key order before the full sort.
          // std::sort is unstable, so re-sorting the already-partially-sorted dstp
          // would land equal-valued keys differently than the reference (which
          // sorts the pristine ascending-key vector once). H32: repro showed every
          // divergence had unsafe=1 -- the fallback itself was the bug.
          dstp = src;
          std::sort(dstp.begin(), dstp.end(), cmp);
        }
      } else {
        std::sort(dstp.begin(), dstp.end(), cmp);
      }
      auto& srow = sorted_ccemb_[(std::size_t)lbl];
      srow.clear();
      srow.reserve(dstp.size());
      for (auto& p : dstp) srow.push_back(p.first);
    }
  }
  f8e_fallbacks_ = fallbacks;

  // Build inv_ccemb_ for F8f Gustavson: key -> list of (dense_cc_idx, val)
  inv_ccemb_.assign((std::size_t)n_, {});
  for (int lbl : label_list) {
    int ci = label_to_idx_[(std::size_t)lbl];
    if (ci < 0) continue;
    const auto& row = ccemb_rows_[(std::size_t)lbl];
    if (row.empty()) continue;
    for (auto& kv : row) {
      inv_ccemb_[(std::size_t)kv.first].emplace_back(ci, kv.second);
    }
  }
}

inline void RFECore::f8f_compute_embs_by_ccs() {
  // Precondition (Etapa 6b item 2): f8e must have run. label_to_idx_ and
  // inv_ccemb_ are both assigned to size N at the END of f8e_compute_ccs_emb;
  // if f8f is called first they are still empty and the accumulation below would
  // silently read an empty inv_ccemb_ (every embycc row zero) instead of failing.
  if (label_to_idx_.size() != (std::size_t)n_ ||
      inv_ccemb_.size() != (std::size_t)n_) {
    throw std::runtime_error(
        "RFE: f8f_compute_embs_by_ccs called before f8e_compute_ccs_emb "
        "(label_to_idx_/inv_ccemb_ not sized N)");
  }
  // ncc = number of dense CC columns = number of DISTINCT ROOTS recorded in
  // label_to_idx_ (singletons INCLUDED: f8e assigns a dense index to every root
  // it collects; a singleton's embycc column is simply all-zero because ccemb
  // skips members.size() <= 1). The former `if (ncc == 0) return;` was dead code
  // -- for n >= 1 there is always >= 1 root -- and its comment wrongly said
  // "non-singleton components".
  int ncc = 0;
  for (int idx : label_to_idx_) if (idx >= 0) ++ncc;

  if (f8_embycc_sparse_) {
    // H33 (Etapa 6b item 6): dense per-THREAD accumulator indexed by DENSE CC idx
    // (0..ncc-1) with a per-row STAMP -- replaces the per-row unordered_map (both
    // the hash overhead and H21's thread_local htab bug). Accumulation order is
    // UNCHANGED: for a fixed ci the products arrive in ascending key c (ra is
    // ascending) exactly as the map version iterated, so the float sum is
    // bit-identical. First touch ASSIGNS (== the reference's map emplace); later
    // touches accumulate. No `==0.0f` visited test -- the stamp IS the membership.
    std::vector<int> row_nnz((std::size_t)n_, 0);
    // Pass 1 (parallel): count distinct dense-CC indices per row via the stamp.
    // No float math, no hashing -- just membership bookkeeping.
    #pragma omp parallel
    {
      std::vector<int> stamp((std::size_t)ncc, 0);
      std::vector<std::pair<idx_t, float>> ra;
      int cur = 0;
      #pragma omp for schedule(static)
      for (int q = 0; q < (int)n_; ++q) {
        ++cur;
        ra.clear();
        emb_.for_each_in_row((idx_t)q, [&](idx_t c, float v) { ra.emplace_back(c, v); });
        int cnt = 0;
        for (auto& [c, vq] : ra)
          for (auto& [ci, vcc] : inv_ccemb_[(std::size_t)c])
            if (stamp[(std::size_t)ci] != cur) { stamp[(std::size_t)ci] = cur; ++cnt; }
        row_nnz[(std::size_t)q] = cnt;
      }
    }
    emb_by_cc_sparse_.row_ptr.assign((std::size_t)n_ + 1, 0);
    for (std::size_t i = 0; i < (std::size_t)n_; ++i)
      emb_by_cc_sparse_.row_ptr[i + 1] = emb_by_cc_sparse_.row_ptr[i] + row_nnz[i];
    const int total_nnz = emb_by_cc_sparse_.row_ptr[(std::size_t)n_];
    emb_by_cc_sparse_.col_idx.resize((std::size_t)total_nnz);
    emb_by_cc_sparse_.values.resize((std::size_t)total_nnz);
    emb_by_cc_sparse_.nrows = (int)n_;
    emb_by_cc_sparse_.ncols = ncc;

    // H16 flag collected OUTSIDE any single row and thrown AFTER the region (X7:
    // no exception escapes an OpenMP region). Negative/non-finite is impossible
    // from the math (non-negative embeddings) but guarded so a regression fails
    // loudly instead of silently propagating into F8g.
    long long h16_bad = 0;
    // Pass 2 (parallel): accumulate into the dense buffer, sort touched ascending
    // (the engine's (row,col) contract), write the CSR row.
    #pragma omp parallel reduction(+:h16_bad)
    {
      std::vector<float> acc((std::size_t)ncc, 0.0f);
      std::vector<int> stamp((std::size_t)ncc, 0);
      std::vector<int> touched;
      touched.reserve(1024);
      std::vector<std::pair<idx_t, float>> ra;
      int cur = 0;
      #pragma omp for schedule(static)
      for (int q = 0; q < (int)n_; ++q) {
        ++cur;
        ra.clear();
        emb_.for_each_in_row((idx_t)q, [&](idx_t c, float v) { ra.emplace_back(c, v); });
        touched.clear();
        for (auto& [c, vq] : ra) {
          for (auto& [ci, vcc] : inv_ccemb_[(std::size_t)c]) {
            if (stamp[(std::size_t)ci] != cur) {
              stamp[(std::size_t)ci] = cur;
              acc[(std::size_t)ci] = vq * vcc;   // first touch: assign (== emplace)
              touched.push_back(ci);
            } else {
              acc[(std::size_t)ci] += vq * vcc;  // subsequent: accumulate
            }
          }
        }
        std::sort(touched.begin(), touched.end());
        const int lo = emb_by_cc_sparse_.row_ptr[(std::size_t)q];
        for (int p = 0; p < (int)touched.size(); ++p) {
          const int ci = touched[(std::size_t)p];
          const float val = acc[(std::size_t)ci];
          if (!(val >= 0.0f) || !std::isfinite(val)) ++h16_bad;
          emb_by_cc_sparse_.col_idx[(std::size_t)(lo + p)] = ci;
          emb_by_cc_sparse_.values[(std::size_t)(lo + p)] = val;
        }
      }
    }
    if (h16_bad > 0) {
      throw std::runtime_error(
          "RFE: F8f H16 violated -- " + std::to_string(h16_bad) +
          " negative/non-finite embycc values (legacy guarantees >= 0 and finite)");
    }
  } else {
    // Variant A: Dense embycc - direct simEmb(emb[q], ccemb[r]) for all q, r
    // Matches legacy's computeEmbsByCCs exactly
    emb_by_cc_dense_.assign((std::size_t)n_ * (std::size_t)n_, 0.0f);

    // For each CC label with >1 member (has ccemb)
    for (int lbl = 0; lbl < (int)n_; ++lbl) {
      int ci = label_to_idx_[(std::size_t)lbl];
      if (ci < 0) continue;  // not a CC with >1 member
      const auto& ccemb_row = ccemb_rows_[(std::size_t)lbl];
      if (ccemb_row.empty()) continue;

      // For each query q, compute simEmb(emb[q], ccemb[lbl])
      for (int q = 0; q < (int)n_; ++q) {
        float sim = 0.0f;
        std::vector<std::pair<idx_t, float>> ra;
        emb_.for_each_in_row((idx_t)q,
                             [&](idx_t c, float v) { ra.emplace_back(c, v); });

        // Merge-join simEmb (legacy algorithm)
        std::size_t ia = 0, ib = 0;
        while (ia < ra.size() && ib < ccemb_row.size()) {
          if (ra[ia].first < ccemb_row[ib].first) ++ia;
          else if (ccemb_row[ib].first < ra[ia].first) ++ib;
          else {
            sim += ra[ia].second * ccemb_row[ib].second;
            ++ia; ++ib;
          }
        }
        emb_by_cc_dense_[(std::size_t)q * (std::size_t)n_ + (std::size_t)lbl] = sim;
      }
    }
  }
}

// ---- RFE.cpp:415-451 -- reRankingByCCs (CELL computation only; tail is F8h) ----
// This is the cell loop and its bookkeeping, split from the tail so it can be
// diffed cell-by-cell against the reference's MX. See ref:981-1021.
//
// NUMERICS (load-bearing, mirrors ref:973-977). newSim is computed in DOUBLE
// arithmetic -- sqrt of the int product (i+1)*(j+1) -> ::sqrt(double) -- and the
// float simEmb result is promoted into that double expression. The division is
// by the UNSIGNED getPosition, so the denominator is an unsigned -> double
// promotion. The result is narrowed to float ONLY on the assignment to
// f8g_matrix_. Reproducing this in float at any point would drift the bit pattern.
//
// WRITE SEMANTICS. Plain assignment (ref:999): when several CCs target the same
// (k1,k2) cell the LAST CC in the std::map ascending-label order wins. No +=, no
// accumulator. There is NO diagonal pin (unlike the CPRR Gram): an untouched cell
// keeps its zero-fill and the tail sorts it as an ordinary zero. getPosition reads
// the rank lists POST-f7b and returns l+2 for an absent item.
//
// CC ORDER & PARALLELISM. The reference iterates ccemb_ (a std::map) in
// ascending-key order; ccemb_ has an entry exactly for the CC labels f8e produced
// ccemb_rows_ for (size()>1 members), so ascending-label order here matches. The
// inner (i,j) nest is serial ascending (== the reference). Cell writes are
// parallelised per k1: each k1's full (i,j) nest runs in ONE thread, replaying the
// CCs that touch k1 in ascending-label order, so the cross-CC last-write-wins for
// every shared cell is exactly the reference's order -- no data race, and the
// float/unsigned arithmetic is identical regardless of thread count.
inline void RFECore::f8g_rerank_cells() {
  // Preconditions (same style as f8e->f8f, Etapa 6b item 2).
  if (label_to_idx_.size() != (std::size_t)n_ ||
      sorted_ccemb_.size() != (std::size_t)n_ ||
      ccemb_rows_.size() != (std::size_t)n_) {
    throw std::runtime_error(
        "RFE: f8g_rerank_cells called before f8e/f8f (label_to_idx_/sorted_ccemb_/"
        "ccemb_rows_ not sized N)");
  }
  if ((f8_embycc_sparse_ && emb_by_cc_sparse_.row_ptr.size() != (std::size_t)n_ + 1) ||
      (!f8_embycc_sparse_ && emb_by_cc_dense_.size() != (std::size_t)n_ * (std::size_t)n_)) {
    throw std::runtime_error(
        "RFE: f8g_rerank_cells called before f8f_compute_embs_by_ccs (embycc not built)");
  }
  // CRITICAL: rebuild position index on POST-F7b rank lists.
  // The F1 build_position_index() used pre-F7b rk_; F7b rewrites rk_ in place.
  // The reference's getPosition scans the live rank lists (post-F7b), so we must
  // rebuild the inverted index here to match. H9 proved the index matches the
  // legacy's linear scan WHEN built from the same lists.
  build_position_index();

  const std::size_t nn = (std::size_t)n_;
  f8g_matrix_.assign(nn * nn, 0.0f);  // zero-fill; NO diagonal pin
  f8g_inc_elems_.assign(nn, {});
  f8g_inc_elems_l_.assign(nn, {});
  f8g_pos_absent_ = 0;

  // For each CC label r (ascending), the top-member rows k1 = sorted_ccemb_[r][i]
  // it writes. We pair each k1 with its INDEX i, because the reference writes in
  // r-major order (r, then i, then j) -- so on a (k1,k2) write-conflict the LAST
  // write is the one with the LARGEST i (ties broken by larger r). Replaying in
  // ascending (i, r) reproduces that last-writer exactly; regrouping by r alone
  // picked the wrong winner (~3.2x diff on 4661 shared cells).
  std::vector<std::vector<std::pair<int, int>>> cc_for_k1(nn);  // (i, r) per k1
  for (int r = 0; r < (int)n_; ++r) {
    if (ccemb_rows_[(std::size_t)r].empty()) continue;  // no ccemb for this label
    const auto& sc = sorted_ccemb_[(std::size_t)r];
    const int lim = std::min(k_, (int)sc.size());  // == min(k, d.size()) per ref:991
    for (int i = 0; i < lim; ++i) {
      const int k1 = sc[(std::size_t)i];
      if (k1 < 0 || k1 >= (int)n_) continue;
      cc_for_k1[(std::size_t)k1].emplace_back(i, r);
    }
  }
  // Ascending (i, r): the last element is the last writer in r-major order.
  for (auto& vec : cc_for_k1)
    std::sort(vec.begin(), vec.end());

  long long absent = 0;
  #pragma omp parallel for schedule(dynamic) reduction(+:absent)
  for (int k1 = 0; k1 < (int)n_; ++k1) {
    for (const auto& ir : cc_for_k1[(std::size_t)k1]) {
      const int i = ir.first;
      const int r = ir.second;
      const auto& sc = sorted_ccemb_[(std::size_t)r];
      const int lim = std::min(k_, (int)sc.size());
      {
        for (int j = 0; j < lim; ++j) {
          const int k2 = sc[(std::size_t)j];
          if (k2 < 0 || k2 >= (int)n_) continue;
          const double sim = (double)sim_embycc(k1, k2);
          const unsigned int pos = (unsigned int)position_of((idx_t)k1, (idx_t)k2);
          if (pos == (unsigned int)(l_ + 2)) ++absent;
          const double newSim =
              (1.0 + std::sqrt((double)(i + 1) * (double)(j + 1)) * sim) / (double)pos;
          f8g_matrix_[nn * (std::size_t)k1 + (std::size_t)k2] = (float)newSim;
          f8g_inc_elems_[(std::size_t)k1].insert(k2);
        }
      }
    }
  }
  f8g_pos_absent_ = absent;

  // incElemsL_: the ids in each q's POST-f7b rank list (ref:1005-1010). Serial.
  for (int q = 0; q < (int)n_; ++q) {
    for (int i = 0; i < l_; ++i) {
      f8g_inc_elems_l_[(std::size_t)q].insert(
          rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i]);
    }
  }
}


// F8h (Etapa 8): the TAIL of reRankingByCCs -- mirrors f7b_rerank_cprr_tail but
// uses f8g_matrix_ (no diagonal pin, unwritten = 0) and f8g_inc_elems_/
// f8g_inc_elems_l_ built by f8g. See ref:1012-1020.
inline void RFECore::f8h_rerank_tail() {
  // Preconditions: f8g must have run and populated the matrix and sets.
  if (f8g_matrix_.empty() || f8g_inc_elems_.size() != (std::size_t)n_ ||
      f8g_inc_elems_l_.size() != (std::size_t)n_) {
    throw std::runtime_error(
        "RFE: f8h_rerank_tail called before f8g_rerank_cells (f8g state missing)");
  }

  std::vector<char> missing_self((std::size_t)n_, 0);
  #pragma omp parallel for schedule(static)
  for (int q = 0; q < (int)n_; ++q) {
    // 1. extras = set_difference(f8g_inc_elems_[q], f8g_inc_elems_l_[q]).
    //    Both are std::set<int> (ascending) -- linear merge.
    std::vector<int> extras;
    std::set_difference(
        f8g_inc_elems_[(std::size_t)q].begin(), f8g_inc_elems_[(std::size_t)q].end(),
        f8g_inc_elems_l_[(std::size_t)q].begin(), f8g_inc_elems_l_[(std::size_t)q].end(),
        std::back_inserter(extras));

    // 2. rkTmp = first l of rk_[q] + extras (extras already ascending by id).
    std::vector<int> rkTmp((std::size_t)l_ + extras.size());
    for (int i = 0; i < l_; ++i) {
      rkTmp[(std::size_t)i] = rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i];
    }
    std::copy(extras.begin(), extras.end(), rkTmp.begin() + l_);

    // 3. a[j] = f8g_matrix_[q * n_ + rkTmp[j]] (NO diagonal pin; unwritten = 0).
    std::vector<float> a(rkTmp.size());
    for (std::size_t j = 0; j < rkTmp.size(); ++j) {
      const int img = rkTmp[j];
      a[j] = (img >= 0 && img < (int)n_)
                 ? f8g_matrix_[(std::size_t)q * (std::size_t)n_ + (std::size_t)img]
                 : 0.0f;
    }

    // 4. Stable DESCENDING sort (shifts on strict `<` -> stable_sort with `>`).
    //    Ties keep rkTmp order (current list first, then extras ascending).
    std::stable_sort(rkTmp.begin(), rkTmp.end(),
                     [&](int u, int v) {
                       const std::size_t iu = (std::size_t)(&u - &rkTmp[0]);
                       const std::size_t iv = (std::size_t)(&v - &rkTmp[0]);
                       return a[iu] > a[iv];
                     });

    // 5. Swap self to position 0 (NOT move-to-front; ref:758-770).
    auto it = std::find(rkTmp.begin(), rkTmp.end(), q);
    if (it != rkTmp.end()) {
      std::iter_swap(rkTmp.begin(), it);
    } else {
      missing_self[(std::size_t)q] = 1;
    }

    // 6. Copy back only first l entries.
    for (int i = 0; i < l_; ++i) {
      rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i] = rkTmp[(std::size_t)i];
    }
  }

  // Report missing self (should not happen if input was validated, but legacy pins it).
  for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
    if (missing_self[q]) {
      throw std::runtime_error("RFE: F8h self missing in row " + std::to_string(q) +
                               " -- legacy pins self during the tail sort");
    }
  }
}


#endif  // RFE_CORE_HPP