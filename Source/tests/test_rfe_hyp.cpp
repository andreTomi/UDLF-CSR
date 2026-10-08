// test_rfe_hyp.cpp -- Step 1 of the RFE port: the hypothesis gate.
//
// Every claim below was a READING of RFE.cpp before any of this code existed.
// This file decides whether those readings survive contact with a running legacy.
// Nothing is asserted because "algebra says so"; each test prints its measured
// number and states an explicit pass criterion.
//
//  H1  emb[q] == S + S*S, where S[q, rk[q][p]] = rankKScore(p+1), p < k.
//      T1  cell-by-cell against an INDEPENDENT reference built from the
//          definition.
//      T1b the ORDER claim: the plan says accumulation is by pos1 (rank
//          position), interleaved term1/term2, and that a column-ordered
//          SpGEMM replays a DIFFERENT permutation. Counts the divergences; a
//          count of 0 means the plan was wrong about the order.
//
//  H2  emb carries keys whose value is EXACTLY 0.0f, and they are load-bearing.
//      T2  count exact-zero keys and how many survive into incElems.
//      T2b the engine filters emission at |v| > 1e-9 (SparseMatrix.hpp:559).
//          Count the legacy keys that filter would DISCARD -- this decides
//          whether multiply_and_prune can build emb at all.
//
//  H3  remb == emb^T exactly.      T3  bit equality + key-set comparison.
//  H4  combineReverseEmb is NOT combine() and NOT merge(): the legacy KEEPS the
//      key and writes 0.0f, the engine returns the intersection. T4 compares
//      supports and reports the ULP distance on shared cells.
//  H5  simEmb is reproducible by a two-pointer merge over two CSR rows (std::map
//      iterates ascending, CSR columns are ascending). T5: ZERO ULP required.
//  H6  ee[q] is INVARIANT to the sorted_emb tie-break. T6: bit equality against
//      a deliberately perturbed tie order.
//  H7  the Gram accumulated in sorted_ee order matches the legacy cell-by-cell
//      (excluding the pow(2,30) diagonal pin). T7 is the BASELINE the replay
//      kernel must hit; it is not a test of multiply_and_prune.
//  H8  pruning the Gram to top-L per row, with the legacy's exact tie key,
//      preserves the top-L of the union. T8 rebuilds the rows and compares.
//      MEASURED FINDING, and it constrains Step 2: 26.56% of incElems cells have
//      Gram value EXACTLY 0.0f (incElems is inserted before the product is
//      known, RFE.cpp:710). A value-only top-L prune drops them, so it is NOT
//      exact in general. H8 asserts exactness only when that count is zero and
//      otherwise REPORTS it -- the port must prune over the UNION of (nonzero
//      Gram) and (zero-valued incElems).
//  H9  a position INDEX reproduces getPosition exactly, duplicates and the
//      absent (l+2) case included. T9 over every (q,i) pair.
//
// Two assertions are deliberately REGIME-scoped (H0), because a saturated
// fixture cannot test them: at L == N the support saturates, ~0 of the emb keys
// are exactly zero, and combine()/merge() then have nothing to drop. See the
// H0 block for the measurement.
//
// POSTURE: -O2, -ffp-contract=off, no fast-math, same as the Step 0 golden.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/tests
//       Source/tests/test_rfe_hyp.cpp -o /tmp/test_rfe_hyp

#include "Core/hpp/SparseMatrix.hpp"
#include "rfe_ref_verbatim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg)                                                   \
  do {                                                                      \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
      std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);               \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

namespace {

// ---- fixture-derived helpers ----------------------------------------------
// rankKScore must be the legacy's expression, not a re-derived one: it is
// 1 - log(pos)/log(k), and at pos==k that is EXACTLY 0.0f because log(k)/log(k)
// is exactly 1.0 in IEEE. Re-deriving it as (log(k)-log(pos))/log(k) would give
// the same value here, but a different bit pattern in general, and the whole
// point of this gate is to keep the legacy's expression.
float rankKScore_legacy(int pos, int k) { return (float)(1 - (log(pos) / log(k))); }

// Build a CSRMatrix from a std::map<int,std::map<int,float>> row-per-key
// collection. Values are copied VERBATIM, including exact zeros and with NO
// |v| filter -- the filter is the engine's, applied inside multiply_and_prune,
// and applying it here would hide the very thing H2b measures.
M map_to_csr(const std::map<int, std::map<int, float>>& rows, int n, int m) {
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
    ranges[(std::size_t)tid] = {(idx_t)r0, (idx_t)r1};
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

struct Ulp { long diff; double rel; double abs_; };
Ulp ulp_of(float a, float b) {
  Ulp u{0, 0.0, 0.0};
  if (a == b) return u;                       // includes both-exact-zero
  int32_t ia, ib;
  std::memcpy(&ia, &a, 4);
  std::memcpy(&ib, &b, 4);
  if (ia < 0) ia = (int32_t)0x80000000 - ia;
  if (ib < 0) ib = (int32_t)0x80000000 - ib;
  u.diff = (long)(ia - ib);
  u.abs_ = (double)a - (double)b;
  u.rel = (a != 0.0f) ? std::fabs((double)(u.abs_ / a)) : 0.0;
  return u;
}

}  // namespace

// ===========================================================================
// H1 -- emb == S + S*S, and the ORDER claim
// ===========================================================================
// Reference built straight from the definition of S, in the legacy's OWN order
// (pos1 ascending, term1 before the k terms 2 of the same pos1). If the legacy
// matches this bit-for-bit, the decomposition and the order are both confirmed
// in one shot -- the reference is not an independent reimplementation of the
// order, it IS the legacy's order, which is the point.
namespace {

std::map<int, float> ref_emb_pos1_order(const std::vector<int>& rk, int n, int l,
                                        int q, int k) {
  std::map<int, float> e;
  for (int pos1 = 0; pos1 < k; ++pos1) {
    const int img_n = rk[(std::size_t)l * q + pos1];
    const float wn = rankKScore_legacy(pos1 + 1, k);
    auto it = e.find(img_n);
    if (it != e.end()) it->second += wn; else e[img_n] = wn;
    for (int pos2 = 0; pos2 < k; ++pos2) {
      const float wnn = rankKScore_legacy(pos2 + 1, k);
      const int img_nn = rk[(std::size_t)l * img_n + pos2];
      const float v = wn * wnn;
      auto jt = e.find(img_nn);
      if (jt != e.end()) jt->second += v; else e[img_nn] = v;
    }
  }
  return e;
}

// T1b variant: "all of S, then all of S*S". This is the WRONG order for parity
// but it is the tempting one to write, so the gate measures how far off it is.
std::map<int, float> ref_emb_split_order(const std::vector<int>& rk, int n, int l,
                                         int q, int k) {
  std::map<int, float> s, ss;
  for (int pos1 = 0; pos1 < k; ++pos1) {
    const int img_n = rk[(std::size_t)l * q + pos1];
    const float wn = rankKScore_legacy(pos1 + 1, k);
    auto it = s.find(img_n);
    if (it != s.end()) it->second += wn; else s[img_n] = wn;
    for (int pos2 = 0; pos2 < k; ++pos2) {
      const float wnn = rankKScore_legacy(pos2 + 1, k);
      const int img_nn = rk[(std::size_t)l * img_n + pos2];
      const float v = wn * wnn;
      auto jt = ss.find(img_nn);
      if (jt != ss.end()) jt->second += v; else ss[img_nn] = v;
    }
  }
  for (const auto& kv : ss) {
    auto it = s.find(kv.first);
    if (it != s.end()) it->second += kv.second; else s[kv.first] = kv.second;
  }
  return s;
}

long diff_cells(const std::map<int, float>& a, const std::map<int, float>& b) {
  long d = 0;
  std::set<int> keys;
  for (const auto& kv : a) keys.insert(kv.first);
  for (const auto& kv : b) keys.insert(kv.first);
  for (int key : keys) {
    const auto ia = a.find(key);
    const auto ib = b.find(key);
    const float va = (ia == a.end()) ? 0.0f : ia->second;
    const float vb = (ib == b.end()) ? 0.0f : ib->second;
    if (ulp_of(va, vb).diff != 0) ++d;
  }
  return d;
}

long key_set_diff(const std::map<int, float>& a, const std::map<int, float>& b) {
  long d = 0;
  std::set<int> keys;
  for (const auto& kv : a) keys.insert(kv.first);
  for (const auto& kv : b) keys.insert(kv.first);
  for (int key : keys) {
    const bool ia = a.count(key) != 0;
    const bool ib = b.count(key) != 0;
    if (ia != ib) ++d;
  }
  return d;
}

// ===========================================================================
// H3 -- remb == emb^T exactly
// ===========================================================================
// The plan called this the one phase that maps straight onto CSRMatrix::transpose.
// That is only true if remb's key set matches emb's COLUMN set. It need not: a
// column whose only contributions are exact zeros still creates a key in remb
// (incEmb's else-branch, RFE.cpp:692) but emb may never MENTION that row -- so
// remb can have rows that emb does not, and (with emb's row always present)
// remb's rows are a subset of the ids. The test therefore checks VALUES on the
// intersection and REPORTS the key-set asymmetry instead of asserting equality.
std::map<int, std::map<int, float>> legacy_emb_pre;  // filled in main
std::map<int, std::map<int, float>> legacy_remb_pre;

}  // namespace

// The tranche-2 helpers (H5..H8) were appended INSIDE main() during development,
// which is a syntax error (a function cannot be defined inside a function, and a
// namespace cannot open inside one). They now live here, at file scope, with
// internal linkage. Forward declarations first, then the definitions.
namespace {

struct Cand { float v; bool in_list; int pos_in_list; int id; };

using Row = std::vector<std::pair<idx_t, float>>;
static Row row_of(const M& m, int r);
static float sim_emb_merge(const Row& a, const Row& b);
static float ee_sum_in_order(const std::map<int, float>& row,
                             const std::vector<int>& order, int k);
static std::vector<float> gram_reference(const std::map<int, std::map<int, float>>& emb,
                                         const std::vector<float>& ee,
                                         const std::vector<int>& sorted_ee, int n);
static bool cand_less(const Cand& a, const Cand& b);

// ========================= helper DEFINITIONS =============================

static Row row_of(const M& m, int r) {
  Row row;
  row.reserve((std::size_t)m.row_nnz((idx_t)r));
  m.for_each_in_row((idx_t)r, [&](idx_t c, float v) { row.push_back({c, v}); });
  return row;
}

// Two-pointer merge over two ascending-column rows. Column order IS the point:
// it is what makes the accumulation order match std::map's ascending iteration.
static float sim_emb_merge(const Row& a, const Row& b) {
  float sim = 0.0f;
  std::size_t ia = 0, ib = 0;
  while (ia < a.size() && ib < b.size()) {
    if (a[ia].first == b[ib].first) {
      sim += a[ia].second * b[ib].second;
      ++ia; ++ib;
    } else if (a[ia].first < b[ib].first) {
      ++ia;
    } else {
      ++ib;
    }
  }
  return sim;
}

// Re-sum the first k keys of a (possibly perturbed) key order, mirroring the
// legacy's loop: pos++; if (pos > k) break; eeValue += emb[q][key];
static float ee_sum_in_order(const std::map<int, float>& row,
                             const std::vector<int>& order, int k) {
  float ee = 0.0f;
  int pos = 0;
  for (int key : order) {
    ++pos;
    if (pos > k) break;
    const auto it = row.find(key);
    if (it != row.end()) ee += it->second;
  }
  return ee;
}

// The Gram: sum over q in sorted_ee of (emb[q][i]*emb[q][j]) * ee[q] * 1.
// wj is the LITERAL 1 (RFE.cpp:561), so the association is (vi*vj)*wq*1.0f.
static std::vector<float> gram_reference(const std::map<int, std::map<int, float>>& emb,
                                         const std::vector<float>& ee,
                                         const std::vector<int>& sorted_ee, int n) {
  std::vector<float> g((std::size_t)n * (std::size_t)n, 0.0f);
  for (int q : sorted_ee) {
    const auto row = emb.find(q);
    if (row == emb.end()) continue;
    const float wq = ee[(std::size_t)q];
    for (const auto& i_kv : row->second) {
      const int i = i_kv.first;
      const float vi = i_kv.second;
      for (const auto& j_kv : row->second) {
        const int j = j_kv.first;
        const float vj = j_kv.second;
        g[(std::size_t)i * (std::size_t)n + (std::size_t)j] += (vi * vj) * wq * 1.0f;
      }
    }
  }
  return g;
}

// Total order implied by the legacy's STABLE insertion sort, most significant
// first: value DESC, then current-list members ahead of extras, then list
// position, then ascending id.
static bool cand_less(const Cand& a, const Cand& b) {
  if (a.v != b.v) return a.v > b.v;
  if (a.in_list != b.in_list) return a.in_list;
  if (a.in_list) return a.pos_in_list < b.pos_in_list;
  return a.id < b.id;
}

}  // namespace

int main(int argc, char** argv) {
  // Fixture: same shape as the Step 0 golden (self at 0, rows are permutations),
  // same generator, so a regression here is comparable with that golden rather
  // than a different world. argv override: N L k t seed.
  const int N    = (argc > 1) ? std::atoi(argv[1]) : 120;
  const int L    = (argc > 2) ? std::atoi(argv[2]) : 30;
  const int k    = (argc > 3) ? std::atoi(argv[3]) : 8;
  const int t    = (argc > 4) ? std::atoi(argv[4]) : 2;
  const int seed = (argc > 5) ? std::atoi(argv[5]) : 42;
  const double pa = 0.1;

  std::printf("RFE hypothesis gate -- Step 1\n");
  std::printf("fixture N=%d L=%d k=%d t=%d pa=%.3f seed=%d\n\n", N, L, k, t, pa, seed);

  std::vector<int> rk_in((std::size_t)N * (std::size_t)L, 0);
  {
    std::mt19937 rng((unsigned)seed);
    std::vector<int> perm((std::size_t)N);
    for (int i = 0; i < N; ++i) perm[(std::size_t)i] = i;
    for (int i = 0; i < N; ++i) {
      std::shuffle(perm.begin(), perm.end(), rng);
      auto it = std::find(perm.begin(), perm.end(), i);
      std::swap(perm[0], *it);
      for (int j = 0; j < L; ++j)
        rk_in[(std::size_t)i * (std::size_t)L + (std::size_t)j] = perm[(std::size_t)j];
    }
  }

  // TWO legacy instances, deliberately:
  //   ref_pre : rankNorm then create_emb ONLY. H1/H2 need emb as createEmb leaves
  //             it -- combineReverseEmb mutates it in place, so the pre-combine
  //             state is unrecoverable once the full phase has run.
  //   ref_t   : rankNorm then hyper_rr_embedding (adds the reverse + combine).
  //             H3/H4/H5 need the post-combine emb.
  RFERef ref_pre(N, L, k, t, pa);
  ref_pre.load_rk(rk_in);
  ref_pre.rank_norm();
  ref_pre.create_emb();

  RFERef ref_t(N, L, k, t, pa);
  ref_t.load_rk(rk_in);
  ref_t.rank_norm();
  ref_t.hyper_rr_embedding();

  const std::map<int, std::map<int, float>>& emb_pre = ref_pre.emb();
  const std::map<int, std::map<int, float>>& remb_pre = ref_t.remb();
  const std::map<int, std::map<int, float>>& emb_post = ref_t.emb();
  const std::vector<int>& rk_now = ref_pre.rk();

  // Hoisted so the regime gate below can read the census from H2/H4. Declared
  // here rather than relying on block scope; H2/H4 assign them.
  long h2_total = 0, h2_zeros = 0, h4_lost_combine = 0, h4_lost_merge = 0;
  // Hoisted for the RESULT line: H8's exactness assertion is skipped when this is
  // nonzero, and the summary must say so rather than print a bare count.
  long h8_zero_incelems = 0;

  // ---- H1: emb == S + S*S, in the legacy's own pos1 order --------------------
  {
    long cells = 0, diff_pos1 = 0, diff_split = 0, keydiff_pos1 = 0, keydiff_split = 0;
    for (int q = 0; q < N; ++q) {
      const std::map<int, float>& got = emb_pre.at(q);
      const std::map<int, float> bypos1 = ref_emb_pos1_order(rk_now, N, L, q, k);
      const std::map<int, float> bysplit = ref_emb_split_order(rk_now, N, L, q, k);
      cells += (long)got.size();
      diff_pos1 += diff_cells(got, bypos1);
      keydiff_pos1 += key_set_diff(got, bypos1);
      diff_split += diff_cells(got, bysplit);
      keydiff_split += key_set_diff(got, bysplit);
    }
    std::printf("H1  emb cells=%ld | pos1-order: ulp=%ld keydiff=%ld"
                " | split(S then S*S): ulp=%ld keydiff=%ld\n",
                cells, diff_pos1, keydiff_pos1, diff_split, keydiff_split);
    CHECK(diff_pos1 == 0, "H1: emb == S + S*S bit-exact in pos1 order");
    CHECK(keydiff_pos1 == 0, "H1: same key set as the pos1-order reference");
    CHECK(keydiff_split == 0, "H1b: split order has the same key set (values may differ)");
    std::printf("H1b order probe: split-order value divergence = %ld cells%s\n",
                diff_split,
                diff_split == 0
                    ? "  (INCONCLUSIVE: the two orders coincide on this fixture)"
                    : "  (order is observable on this fixture)");
  }

  // ---- H2: exact-zero keys, and what the engine's 1e-9 filter would discard ----
  {
    long total = 0, zero_keys = 0, below_1e9 = 0, neg = 0;
    double smallest_nonzero = 1e300;
    for (int q = 0; q < N; ++q) {
      for (const auto& kv : emb_pre.at(q)) {
        ++total;
        if (kv.second == 0.0f) { ++zero_keys; continue; }
        if (kv.second < 0.0f) ++neg;
        const double a = std::fabs((double)kv.second);
        if (a < smallest_nonzero) smallest_nonzero = a;
        if (!(a > 1e-9)) ++below_1e9;   // engine filter, SparseMatrix.hpp:559
      }
    }
    const double tot_d = (double)(total ? total : 1);
    std::printf("H2  emb keys=%ld exact-zero=%ld (%.2f%%) nonzero-with-|v|<=1e-9=%ld"
                " smallest-nonzero=%.4e\n",
                total, zero_keys, 100.0 * (double)zero_keys / tot_d, below_1e9,
                smallest_nonzero);
    std::printf("H2b a port emitting emb through multiply_and_prune would DISCARD"
                " %ld of %ld keys (%.2f%%)\n",
                zero_keys + below_1e9, total,
                100.0 * (double)(zero_keys + below_1e9) / tot_d);
    CHECK(neg == 0, "H2: no negative emb values");
    h2_total = total;
    h2_zeros = zero_keys;   // consumed by the regime gate after H4
  }

  // ---- H3: remb == emb^T -----------------------------------------------------
  {
    long cells = 0, ulp = 0, keydiff = 0;
    for (int q = 0; q < N; ++q) {
      const auto rit = remb_pre.find(q);
      if (rit == remb_pre.end()) continue;
      for (const auto& kv : rit->second) {
        const int col = kv.first;          // remb[q][col] should equal emb[col][q]
        ++cells;
        const auto eit = emb_pre.find(col);
        const bool present = (eit != emb_pre.end()) && (eit->second.count(q) != 0);
        const float want = present ? eit->second.at(q) : 0.0f;
        if (!present) ++keydiff;
        if (ulp_of(kv.second, want).diff != 0) ++ulp;
      }
    }
    std::printf("H3  remb cells=%ld  ulp-vs-emb-transpose=%ld  keys-without-counterpart=%ld\n",
                cells, ulp, keydiff);
    CHECK(ulp == 0, "H3: every remb cell equals emb[col][row] bit-exactly");
  }
// ---- H4: combineReverseEmb is NOT combine() and NOT merge() ----------------
  // The legacy KEEPS every key of emb[q] and writes 0.0f where the reverse is
  // absent. CSRMatrix::combine returns the intersection of NONZERO supports, so
  // it LOSES those keys. CSRMatrix::merge covers the union but its emission
  // filter `if (res != T{})` (SparseMatrix.hpp:675/682/701) drops the zeros --
  // also losing them. The port therefore needs a DEDICATED kernel, and this is
  // the measurement that shows it.
  {
    const M emb_pre_csr = map_to_csr(emb_pre, N, N);
    const M emb_post_csr = map_to_csr(emb_post, N, N);
    const M remb_csr = map_to_csr(remb_pre, N, N);
    const M comb = M::combine(emb_pre_csr, remb_csr);
    const M mrg = M::merge(emb_pre_csr, remb_csr,
                            [](float a, float b) { return a * b; });

    long legacy_keys = 0, post_keys = 0, combine_keys = 0, merge_keys = 0;
    long lost_by_combine = 0, lost_by_merge = 0, value_mismatch_combine = 0;
    long max_ulp_combine = 0;
    // Key loss must be measured as a KEY-SET difference, not a value
    // comparison. The first version of this test compared values and reported
    // "lost 0" while combine() had in fact dropped 3443 of 5461 keys -- because
    // every lost key's legacy value is EXACTLY 0.0f, so the guard
    // `cv == 0 && kv.second != 0` was never true. get() cannot distinguish
    // "absent" from "present with value 0", so the key sets are compared
    // directly by walking each engine row.
    auto engine_row_keys = [](const M& m, int q) {
      std::set<int> s;
      m.for_each_in_row((idx_t)q, [&](idx_t col, float) { s.insert((int)col); });
      return s;
    };
    for (int q = 0; q < N; ++q) {
      legacy_keys += (long)emb_pre.at(q).size();
      post_keys    += (long)emb_post.at(q).size();
      combine_keys += (long)comb.row_nnz((idx_t)q);
      merge_keys   += (long)mrg.row_nnz((idx_t)q);

      const std::set<int> ckeys = engine_row_keys(comb, q);
      const std::set<int> mkeys = engine_row_keys(mrg, q);
      for (const auto& kv : emb_post.at(q)) {
        const int col = kv.first;
        // The legacy keeps the key; does the engine?
        if (!ckeys.count(col)) {
          ++lost_by_combine;
          if (!mkeys.count(col)) ++lost_by_merge;
        }
        // Value agreement on the keys both have (this is the ULP question).
        if (ckeys.count(col)) {
          const float cv = comb.get((idx_t)q, (idx_t)col);
          if (ulp_of(cv, kv.second).diff != 0) {
            ++value_mismatch_combine;
            const long d = ulp_of(cv, kv.second).diff;
            if (d > max_ulp_combine) max_ulp_combine = d;
          }
        }
      }
    }
    std::printf("H4  legacy post-combine keys=%ld (pre-combine %ld)\n", post_keys, legacy_keys);
    std::printf("H4b combine() kept %ld keys, merge() kept %ld keys"
                " -> combine lost %ld, merge lost %ld\n",
                combine_keys, merge_keys, lost_by_combine, lost_by_merge);
    std::printf("H4c value mismatches combine-vs-legacy=%ld (max ulp=%ld)\n",
                value_mismatch_combine, max_ulp_combine);
    // The claim: the legacy's support SURVIVES the combine, the engine's does not.
    CHECK(post_keys == legacy_keys,
          "H4: combineReverseEmb preserves the key set of emb (support unchanged)");
    h4_lost_combine = lost_by_combine;
    h4_lost_merge   = lost_by_merge;
  }

  // ---- H0: the regime gate over H2 and H4b --------------------------------
  // H2 ("emb carries exact zeros") and H4b ("combine/merge must lose keys") are
  // both REGIME-DEPENDENT, and the sweep is what proves it. At L == N -- the same
  // saturation boundary Step 0's step0_probe_saturated.sh measured -- emb is
  // effectively DENSE, every column receives contributions from many pos1 values,
  // and `acc += 0.0f` stops being any column's final value: measured 0 exact
  // zeros out of 3600 keys. With no zeros, combine()/merge() have nothing to drop
  // either, so H4b cannot fire.
  //
  // Both are therefore asserted ONLY in the sparse regime and SKIPPED loudly
  // otherwise. Asserting them unconditionally is what made the L==N sweep row
  // fail here -- the legacy was right, the check was over-specified. Briefing rule
  // 2 (synthetic density lies) recurring inside Step 1, in the same shape as Step 0.
  const double sat_all = (double)h2_total / ((double)N * (double)N);
  const bool sparse_all = (sat_all < 0.90);
  std::printf("H0  emb saturation |supp|/n^2 = %.4f (%s regime)\n", sat_all,
              sparse_all ? "SPARSE" : "SATURATED");
  if (sparse_all) {
    CHECK(h2_zeros > 0, "H2 [sparse]: emb carries keys with value exactly 0.0f");
    CHECK(h4_lost_combine > 0,
          "H4b [sparse]: CSRMatrix::combine DOES lose keys the legacy keeps");
    CHECK(h4_lost_merge > 0, "H4b [sparse]: CSRMatrix::merge ALSO loses them");
  } else {
    std::printf("  SKIP: H2/H4b not asserted -- fixture SATURATED (sat=%.4f);"
                " measured zeros=%ld/%ld, lost_by_combine=%ld, lost_by_merge=%ld."
                " A Step 3 real-data question (production has L << N).\n",
                sat_all, h2_zeros, h2_total, h4_lost_combine, h4_lost_merge);
  }
  std::printf("H0b density caveat: H2/H4b numbers above are regime-dependent."
              " Production ALOI is N=72000 L=400 (L/N = %.5f) -- re-measure there in"
              " Step 3 before treating any of these as universal.\n",
              400.0 / 72000.0);

  // ---- H9: a position index reproduces getPosition exactly ------------------
  // The legacy scans O(L) per call and returns l+2 when the id is absent. A port
  // wants an inverted index. The subtlety the plan flagged: getPosition returns
  // the FIRST match, so the index must keep the first occurrence only -- and the
  // absent case must stay l+2, not 0.
  {
    // Inverted index: for each image id, the ascending list of (row, pos).
    // GUARD: the fixture is a permutation of [0,N) so img is in range, but an
    // out-of-range id here would corrupt the heap and surface as a segfault
    // several blocks later (in H5) -- far from the actual cause. Bounds-check
    // loudly so a bad id is reported where it happens.
    std::vector<std::vector<std::pair<int, int>>> inv((std::size_t)N);
    long oob = 0;
    for (int q = 0; q < N; ++q)
      for (int i = 0; i < L; ++i) {
        const int img = rk_in[(std::size_t)q * (std::size_t)L + (std::size_t)i];
        if (img < 0 || img >= N) { ++oob; continue; }
        inv[(std::size_t)img].push_back({q, i});   // built in ascending (q,i)
      }
    CHECK(oob == 0, "H9: every fixture id is in [0,N) (index bound)");
    // getPosition(qImg, img) scans ROW qImg for the VALUE img. rankNorm calls
    // it as getPosition(img_rl_i, q): "where does q sit in img_rl_i's list".
    //
    // Index direction matters and the first version had it BACKWARDS: it looked
    // up inv[img_rl_i] (which answers "who lists img_rl_i") instead of inv[q]
    // (which answers "who does q list"). That is why it reported 3446/3600
    // mismatches. inv[q] holds (row, pos) pairs with rk[row][pos] == q, so the
    // entry with row == img_rl_i is exactly the reciprocal rank we want.
    long checked = 0, mismatch = 0, absent = 0;
    RFERef probe(N, L, k, t, pa);
    probe.load_rk(rk_in);
    for (int q = 0; q < N; ++q) {
      for (int i = 0; i < L; ++i) {
        const int img_rl_i = rk_in[(std::size_t)q * (std::size_t)L + (std::size_t)i];
        const unsigned legacy_pos = probe.get_position(img_rl_i, q);
        unsigned idx_pos = (unsigned)(L + 2);
        // Scan row img_rl_i's list for the value q == inv[q] entry with
        // row == img_rl_i. First match wins, matching the legacy's early return.
        for (const auto& e : inv[(std::size_t)q])
          if (e.first == img_rl_i) { idx_pos = (unsigned)(e.second + 1); break; }
        if (legacy_pos == (unsigned)(L + 2)) ++absent;
        ++checked;
        if (legacy_pos != idx_pos) ++mismatch;
      }
    }
    std::printf("H9  getPosition pairs=%ld absent(=%d)=%ld index-mismatches=%ld\n",
                checked, L + 2, absent, mismatch);
    CHECK(checked > 0, "H9: exercised a nonzero number of getPosition pairs");
    CHECK(mismatch == 0, "H9: inverted index reproduces getPosition exactly (first match, l+2 absent)");
  }

  // ---- H5 -- simEmb is reproducible by a two-pointer merge over two CSR rows ---
// The claim: simEmb iterates di (emb[q]) in ASCENDING KEY ORDER and looks each
// key up in dj (RFE.cpp:677-686). A CSR row stores columns strictly ascending, so
// a two-pointer merge visits the same (key, value) pairs in the same order and
// accumulates the same partial sums -- hence ZERO ULP, not a tolerance. That is
// what makes reRankingByEmbSim portable without a bespoke masked kernel: the mask
// (the rank list) only selects WHICH pairs get written, never reorders the sum.
//
// The port computes simEmb(emb[q], emb[img_i]) / (i+1) for the L members of row
// q's rank list. This test compares that merge against the legacy's own simEmb
// over EVERY pair the phase actually touches -- the same set reRankingByEmbSim
// visits, so there is no smaller sample to argue about.
//
// Rows are materialised through the PUBLIC API only: for_each_in_row yields
// columns in ascending order (SparseMatrix.hpp contract), so collecting a row
// into (col,val) pairs preserves that order. No raw row_ptr_/col_idx_ access --
// the test stays inside the public contract the port will also use.
//
// (row_of / sim_emb_merge / ee_sum_in_order / gram_reference / cand_less are
// DEFINED just above main(); only their declarations are needed here.)

// ---- H6 -- ee[q] is INVARIANT to the sorted_emb tie-break -----------------
// The claim: sortEmb uses std::sort (UNSTABLE) by value descending, so the order
// of EQUAL-valued keys is an implementation detail. effectEstimationEmb then sums
// the first k of that order. If ties are ADJACENT and IEEE addition is
// commutative (a+b == b+a), the SUM of the top-k is unchanged by any permutation
// among equals, so a partial selection may replace the full sort.
//
// The test does NOT assume that; it MEASURES it. It re-sums the same multiset in
// a deliberately perturbed tie order (reversing each run of equal values) and
// requires bit equality. If float addition turned out non-commutative under this
// summation pattern, the claim would fail here and the port would be forced to
// reproduce std::sort's exact tie order -- which is exactly what we want to know
// BEFORE Step 2 picks a kernel.

// Re-sum the first k keys of a PERMUTED key order, mirroring the legacy's loop:
//   pos++; if (pos > k) break; eeValue += emb[q][key];
// `order` is the tie-perturbed key sequence.

// ---- H7 -- the Gram baseline, accumulated in sorted_ee order --------------
// The claim: reRankingByCPRR computes, for every cell (i,j),
//     matrix[i][j] = SUM over q in sorted_ee of (emb[q][i]*emb[q][j]) * ee[q] * 1
// (RFE.cpp:559-561, 696-715), and the ORDER is sorted_ee -- which is an
// unstable std::sort over ee. This is the BASELINE the replay kernel must hit
// bit-for-bit; it is NOT a test of multiply_and_prune (that comparison is H7b,
// deferred because it needs the pruning decision settled first).
//
// The diagonal is excluded: the legacy overwrites matrix[n*q+q] with pow(2,30)
// AFTER all accumulation (RFE.cpp:572), so (i,i) is not part of the Gram.
//
// wj is the literal 1, not ee[qj] -- the float sequence is (vi*vj)*wi*1, so the
// reference must multiply in that exact association.

// ---- H8 -- the top-L pruning lemma -----------------------------------------
// This is the claim that makes the port's memory bounded, so it is the one with
// the heaviest proof obligation. Stated precisely:
//
//   PRUNE-TOPSOUND. If, for each row q, one keeps only the top L cells of the
//   Gram under the order
//       (value DESC, in-current-ranklist DESC, position-in-list ASC, id ASC)
//   then the final top-L of (current row UNION discovered candidates) is
//   UNCHANGED.
//
// Why that key, and not "|v| large": kernelSortRankedLists_extra builds rkTmp as
// [current list in its existing order] ++ [extras in ascending id], then runs an
// INSERTION SORT that shifts only on strict `<` (RFE.cpp:804) -- i.e. it is
// STABLE. A stable sort's output is determined by (key, original position). The
// current-list members therefore outrank extras at equal value, and among extras
// the smaller id wins. That is exactly the 4-tuple above. Taking the top L of the
// Gram under THIS key preserves the top L of the union, because the union's top-L
// members are each either in the current list (kept) or an extra that must rank
// within the top L of the Gram row (kept) -- the standard top-L-of-a-union lemma,
// under a single total order.
//
// The test does not take the lemma on faith: it BUILDS the pruned rows with that
// key, rebuilds the final top-L, and compares to the legacy's actual output rows.
// A disagreement is not a tolerance question -- it means the port would emit
// different rank lists.

// ---- H5: merge-join == the legacy's simEmb, ZERO ULP -----------------------
  {
    // emb AFTER the reverse+combine, i.e. what reRankingByEmbSim actually reads.
    const M emb_csr = map_to_csr(emb_post, N, N);
    // The rank list must be the one that COEXISTS with emb_post -- i.e. ref_t's,
    // after its own rankNorm + hyper_rr_embedding. rk_now belongs to ref_pre,
    // which stopped at create_emb and never ran the re-rank that re-sorts the
    // lists, so pairing it with emb_post reads ids the two pipelines never saw
    // together. The first version did exactly that and segfaulted on
    // emb_post.at(img_i): the id was in neither map's key set.
    const std::vector<int>& rk_t = ref_t.rk();
    long pairs = 0, ulp = 0, max_ulp = 0;
    for (int q = 0; q < N; ++q) {
      const Row rq = row_of(emb_csr, q);
      for (int i = 0; i < L; ++i) {
        const int img_i = rk_t[(std::size_t)q * (std::size_t)L + (std::size_t)i];
        // A missing row is not an error here: simEmb takes a std::map REFERENCE,
        // and the legacy's emb[img_i] on a missing key DEFAULT-CONSTRUCTS an
        // empty map (RFE.cpp:655) rather than throwing. Using .at() would segfault
        // on exactly the case the legacy handles gracefully, so mirror it: look up,
        // and fall back to an empty row.
        static const std::map<int, float> empty_row;
        const auto it_q = emb_post.find(q);
        const auto it_i = emb_post.find(img_i);
        const std::map<int, float>& row_q = (it_q == emb_post.end()) ? empty_row : it_q->second;
        const std::map<int, float>& row_i = (it_i == emb_post.end()) ? empty_row : it_i->second;
        const Row ri = row_of(emb_csr, img_i);
        const float legacy_sim = ref_t.sim_emb_pub(row_q, row_i);
        const float merge_sim = sim_emb_merge(rq, ri);
        ++pairs;
        const long d = ulp_of(legacy_sim, merge_sim).diff;
        if (d != 0) { ++ulp; if (d > max_ulp) max_ulp = d; }
      }
    }
    std::printf("H5  simEmb pairs=%ld  ulp-vs-two-pointer-merge=%ld (max %ld)\n",
                pairs, ulp, max_ulp);
    CHECK(pairs > 0, "H5: exercised a nonzero number of simEmb pairs");
    CHECK(ulp == 0, "H5: two-pointer merge reproduces simEmb bit-exactly (0 ULP)");
  }

  // ---- H6: ee is invariant to the sorted_emb tie-break ----------------------
  {
    // ref_t stopped at hyper_rr_embedding, so its sorted_emb_ was never populated
    // (sortEmb() is what resizes it) and sorted_emb()[q] is UB on an empty
    // vector -- this was the H5/H6 segfault, located by ASan at the `.empty()`
    // call. Drive a dedicated instance through effec_estimation() so the order
    // actually exists; the RESULT is identical because effec_estimation does not
    // touch emb or the rank lists (sortEmb + effectEstimationEmb only).
    RFERef ref_e(N, L, k, t, pa);
    ref_e.load_rk(rk_in);
    ref_e.rank_norm();
    ref_e.hyper_rr_embedding();
    ref_e.effec_estimation();

    long rows_checked = 0, ulp = 0, max_ulp = 0, ties_seen = 0;
    for (int q = 0; q < N; ++q) {
      const std::vector<std::vector<int>>& se = ref_e.sorted_emb();
      if (q >= (int)se.size()) continue;
      const std::vector<int>& order = se[q];
      const auto rowit = emb_post.find(q);
      if (rowit == emb_post.end()) continue;
      const std::map<int, float>& row = rowit->second;
      if (order.empty()) continue;
      // Perturbed order: REVERSE every maximal run of equal values. That is the
      // most adversarial legal permutation of the ties -- if the sum is
      // invariant to this, it is invariant to the tie order.
      std::vector<int> perturbed = order;
      std::size_t i = 0;
      while (i < perturbed.size()) {
        std::size_t j = i + 1;
        const auto vit = row.find(perturbed[i]);
        const float v = (vit == row.end()) ? 0.0f : vit->second;
        while (j < perturbed.size()) {
          const auto jt = row.find(perturbed[j]);
          const float w = (jt == row.end()) ? 0.0f : jt->second;
          if (w != v) break;
          ++j;
        }
        if (j - i > 1) ties_seen += (long)(j - i - 1);
        std::reverse(perturbed.begin() + (long)i, perturbed.begin() + (long)j);
        i = j;
      }
      const float a = ee_sum_in_order(row, order, k);
      const float b = ee_sum_in_order(row, perturbed, k);
      ++rows_checked;
      const long d = ulp_of(a, b).diff;
      if (d != 0) { ++ulp; if (d > max_ulp) max_ulp = d; }
    }
    std::printf("H6  rows=%ld  tie-swaps-applied=%ld  ulp-vs-perturbed-tie-order=%ld"
                " (max %ld)\n", rows_checked, ties_seen, ulp, max_ulp);
    CHECK(rows_checked > 0, "H6: exercised a nonzero number of emb rows");
    CHECK(ulp == 0,
          "H6: ee is invariant to the sorted_emb tie-break (partial sort is legal)");
    std::printf("H6b note: sorted_ee IS order-sensitive (same instability, different"
                " use) -- H7 pins its order explicitly, so it is NOT free.\n");
  }
// ---- H7: the Gram baseline matches the legacy, off-diagonal, bit-exactly ----
  std::vector<float> gram;
  {
    // Drive a fresh legacy through to just after the Gram is accumulated and the
    // diagonal pinned, WITHOUT the trailing re-rank, so `matrix` holds the Gram.
    RFERef ref_g(N, L, k, t, pa);
    ref_g.load_rk(rk_in);
    ref_g.rank_norm();
    ref_g.hyper_rr_embedding();
    ref_g.effec_estimation();
    ref_g.cprr();

    gram = gram_reference(emb_post, ref_g.ee(), ref_g.sorted_ee(), N);

    const std::vector<float>& mx = ref_g.matrix();
    long cells = 0, ulp = 0, max_ulp = 0, nonzero = 0;
    double max_rel = 0.0;
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < N; ++j) {
        if (i == j) continue;                       // diagonal is pow(2,30), not Gram
        ++cells;
        const float got = mx[(std::size_t)i * (std::size_t)N + (std::size_t)j];
        const float want = gram[(std::size_t)i * (std::size_t)N + (std::size_t)j];
        if (got != 0.0f) ++nonzero;
        const long d = ulp_of(got, want).diff;
        if (d != 0) {
          ++ulp;
          const long ad = d < 0 ? -d : d;
          if (ad > max_ulp) max_ulp = ad;
          const Ulp u = ulp_of(got, want);
          if (u.rel > max_rel) max_rel = u.rel;
        }
      }
    }
    std::printf("H7  Gram off-diagonal cells=%ld (nonzero %ld)  ulp-vs-sorted_ee-order=%ld"
                " (max %ld, max_rel=%.3e)\n", cells, nonzero, ulp, max_ulp, max_rel);
    std::printf("H7b density: the legacy Gram materialises %.2f%% of the n^2 cells;\n"
                "    a port must PRUNE this (H8) rather than accumulate it.\n",
                100.0 * (double)nonzero / ((double)N * (double)N));
    CHECK(cells > 0, "H7: exercised a nonzero number of Gram cells");
    CHECK(ulp == 0, "H7: Gram in sorted_ee order matches the legacy bit-exactly");
  }
// ---- H8: pruning the Gram to top-L preserves the final top-L --------------
  {
    RFERef ref_c(N, L, k, t, pa);
    ref_c.load_rk(rk_in);
    ref_c.rank_norm();
    ref_c.hyper_rr_embedding();
    ref_c.effec_estimation();
    // Snapshot the rank lists BEFORE cprr(): incElemsL is built from the list as
    // it stands at RFE.cpp:570, i.e. before kernelSortRankedLists_extra rewrites
    // it. Reading ref_c.rk() after cprr() gives the FINAL rows, which is the very
    // thing H8 is trying to predict -- using it would make the test tautological.
    const std::vector<int> rk_c = ref_c.rk();
    ref_c.cprr();

    long rows_compared = 0, row_mismatch = 0, pruned_cells = 0, total_cells = 0;
    int worst_row = -1;
    std::vector<int> row_a, row_b;
    for (int q = 0; q < N; ++q) {
      // The current rank list the extra sort treats as "current" members. It must
      // come from the SAME instance that produced the Gram (ref_c), for the same
      // reason as H5: the list and the Gram have to describe one pipeline state.
      std::vector<int> cur((std::size_t)L);
      for (int i = 0; i < L; ++i)
        cur[(std::size_t)i] = rk_c[(std::size_t)q * (std::size_t)L + (std::size_t)i];

      std::vector<Cand> cands;
      for (int j = 0; j < N; ++j) {
        const float v = gram[(std::size_t)q * (std::size_t)N + (std::size_t)j];
        if (v == 0.0f) continue;                   // not discovered
        ++total_cells;
        const auto it = std::find(cur.begin(), cur.end(), j);
        const bool in_list = (it != cur.end());
        cands.push_back({v, in_list, in_list ? (int)(it - cur.begin()) : 0, j});
      }
      std::stable_sort(cands.begin(), cands.end(), cand_less);
      const std::size_t keep = std::min(cands.size(), (std::size_t)L);
      pruned_cells += (long)keep;
      cands.resize(keep);

      std::vector<Cand> uni;
      // The VALUE for a current-list member must come from the legacy's `matrix`
      // AFTER the diagonal pin, not from my Gram reference. Two reasons, both
      // measured:
      //   * the diagonal: matrix[n*q+q] is overwritten with pow(2,30)
      //     (RFE.cpp:572) AFTER accumulation, so the Gram's diagonal is 0 and
      //     only the pinned matrix knows the self really ranks first;
      //   * untouched cells: a current-list member the Gram never touched reads
      //     the zero-FILL of matrix, which is exactly what the legacy sorts on.
      // Using gram[] for these made a 0.0f list member lose to a nonzero extra,
      // which is the first-diff observed (row 33, pos 1: rebuilt=3 legacy=101).
      const std::vector<float>& mx = ref_c.matrix();
      for (int i = 0; i < L; ++i) {
        const int id = cur[(std::size_t)i];
        const float v = mx[(std::size_t)q * (std::size_t)N + (std::size_t)id];
        uni.push_back({v, true, i, id});
      }
      for (const Cand& c : cands)
        if (!c.in_list) uni.push_back(c);
      std::stable_sort(uni.begin(), uni.end(), cand_less);
      row_a.clear();
      for (std::size_t i = 0; i < uni.size() && (int)i < L; ++i)
        row_a.push_back(uni[i].id);
      const auto self = std::find(row_a.begin(), row_a.end(), q);
      if (self != row_a.end()) std::iter_swap(row_a.begin(), self);

      const std::vector<int>& out = ref_c.rk();
      row_b.assign(out.begin() + (std::size_t)q * (std::size_t)L,
                   out.begin() + (std::size_t)(q + 1) * (std::size_t)L);
      ++rows_compared;
      if (row_a != row_b) {
        ++row_mismatch;
        if (worst_row < 0) worst_row = q;
      }
    }
    std::printf("H8  rows=%ld  mismatched-rows=%ld  discovered-cells=%ld"
                "  pruned-to-topL-cells=%ld (%.1f%%)\n",
                rows_compared, row_mismatch, total_cells, pruned_cells,
                total_cells ? 100.0 * (double)pruned_cells / (double)total_cells : 0.0);

    // A guard that turned out to be NECESSARY, not decorative. incElems is
    // inserted unconditionally at RFE.cpp:710, BEFORE the product is known, so it
    // can contain cells whose Gram value is exactly 0.0f. A value-pruned top-L
    // keeps the L largest NONZERO cells and would drop those, while the legacy
    // still considered them. Measured here: 0 such cells on every fixture tested,
    // so the assertion below is not conditional in practice -- but it IS
    // conditional, because a fixture where it is nonzero would be reporting a real
    // limitation of a value-only prune (Step 2 would have to prune over the union
    // of nonzero-Gram and zero-valued-incElems) and that must not be papered over.
    long incelems_total = 0, zero_valued_in_incelems = 0, rows_with_zero_extra = 0;
    for (int q = 0; q < N; ++q) {
      const std::set<int>& inc = ref_c.inc_elems()[(std::size_t)q];
      long zrow = 0;
      for (int j : inc) {
        ++incelems_total;
        if (gram[(std::size_t)q * (std::size_t)N + (std::size_t)j] == 0.0f) ++zrow;
      }
      zero_valued_in_incelems += zrow;
      if (zrow > 0) ++rows_with_zero_extra;
    }
    h8_zero_incelems = zero_valued_in_incelems;
    std::printf("H8c incElems=%ld, zero-valued in incElems=%ld (%.2f%%) over %d/%d rows\n",
                incelems_total, zero_valued_in_incelems,
                incelems_total ? 100.0 * (double)zero_valued_in_incelems / (double)incelems_total : 0.0,
                rows_with_zero_extra, N);
    // MEASURED AND IMPORTANT: this is NOT zero. On the pinned fixture 26.56% of
    // incElems cells have Gram value exactly 0.0f, over EVERY row. So:
    //   * the "value-only top-L prune" is NOT a correct design in general -- it
    //     silently drops zero-valued candidates that the legacy still considered;
    //   * H8 currently PASSES here only because on this fixture the top-L is
    //     filled by nonzero cells anyway, so the dropped zeros never mattered.
    //     That is luck, not a proof, and it would change with data.
    // The correct Step 2 design is a prune over the UNION of (nonzero Gram) and
    // (zero-valued incElems), under the same tie key. H8 asserts exactness only
    // when there are no zero-valued candidates to lose; otherwise it REPORTS the
    // count, which is the input Step 2 needs. Relaxing this to a tolerance would
    // be exactly the failure mode the briefing forbids -- a prune that changes the
    // rank list is not a ULP detail.
    CHECK(rows_compared > 0, "H8: exercised a nonzero number of rows");
    if (zero_valued_in_incelems == 0) {
      CHECK(row_mismatch == 0,
            "H8: pruning the Gram to top-L under the stable-sort key preserves the output");
    } else {
      std::printf("H8  SKIP exactness: %ld zero-valued incElems cells (%.1f%%) -- a"
                  " value-only prune is NOT exact in general; Step 2 must prune over"
                  " (nonzero Gram UNION zero-valued incElems). H8 passing here does"
                  " not license the value-only design.\n",
                  zero_valued_in_incelems,
                  100.0 * (double)zero_valued_in_incelems / (double)(incelems_total ? incelems_total : 1));
    }
  }
// The count is REGIME-DEPENDENT and DATA-DEPENDENT, deliberately:
  //   * H0 adds 3 assertions in the sparse regime (H2 zero-keys, H4b combine,
  //     H4b merge) and skips them when saturated;
  //   * H8's exactness assertion is SKIPPED when zero-valued incElems cells
  //     exist (measured: 26.56% on the pinned fixture), because a value-only
  //     prune provably cannot be exact then. The skip REPORTS the count; it does
  //     not downgrade the claim to a tolerance.
  // So the honest assertion for the gate is "zero failures", with the count
  // reported. Pinning one number would make the gate lie whenever the regime or
  // the data changes -- which is exactly how a stale golden rots.
  const int expected_min = sparse_all ? 19 : 16;
  std::printf("\nRESULT: %d checks, %d failures (>=%d, %s regime%s)\n",
              g_checks, g_failures, expected_min, sparse_all ? "SPARSE" : "SATURATED",
              (h8_zero_incelems == 0) ? "" : ", H8-exactness-skipped");
  std::printf("(tranche 2 complete: H1..H9 -- all nine hypotheses covered)\n");
  return g_failures == 0 ? 0 : 1;
}