// test_rfe_corner.cpp
//
// Corner-case suite for the RFE CSR core. This file exists because the RFE port
// was missing checklist item 4 ("casos-limite multi-seed") -- BFSTree and RDPAC
// both have a corner suite, RFE did not.
//
// Two kinds of checks, following the RDPAC precedent:
//   A) PARITY: crafted inputs run through BOTH the verbatim reference and the
//      core; every phase must match bitwise.
//   B) CORE-ONLY contract checks: inputs the core must REFUSE. These matter most
//      for RFE, because the legacy does not refuse them -- it reads out of
//      bounds or produces 0/0. The core fails closed instead.
//
// The contract checks are NOT speculative. Each corresponds to a `throw` that
// RFECore.hpp already contains, and to a premise the "grValue > 0 by
// construction" theorem (FINDINGS.md) DEPENDS on. If these throws regressed, that
// theorem would silently lose a premise -- so this suite doubles as the
// theorem's regression guard.
//
//   - k < 2                -> rankKScore divides by log(1) == 0  (0/0, legacy)
//   - k > L                -> createEmb reads rkLists[l*q + pos1] OOB (legacy)
//   - L > N                -> legacy checkParameters
//   - wrong rkLists size   -> the core indexes by stride l
//   - row without its own id -> the legacy's self-scan reads rkTmp[i] BEFORE
//                              testing the bound (RFE.cpp:755, 816): OOB read.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp -I Source/tests
//       Source/tests/test_rfe_corner.cpp -o /tmp/test_rfe_corner && /tmp/test_rfe_corner

#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* msg) {
  ++g_checks;
  if (!cond) {
    std::printf("  [FAIL] %s\n", msg);
    ++g_failures;
  } else {
    std::printf("  [OK]   %s\n", msg);
  }
}

// The generator the Step 0 pins were recorded from: mt19937 + shuffle, self
// pinned at index 0, so parity cases use the SAME bytes as the pins.
std::vector<int> gen_fixture(int seed, int N, int L) {
  std::vector<int> rk((std::size_t)N * (std::size_t)L, 0);
  std::mt19937 rng((unsigned)seed);
  std::vector<int> perm((std::size_t)N);
  for (int i = 0; i < N; ++i) perm[(std::size_t)i] = i;
  for (int i = 0; i < N; ++i) {
    std::shuffle(perm.begin(), perm.end(), rng);
    auto it = std::find(perm.begin(), perm.end(), i);
    std::swap(perm[0], *it);
    for (int j = 0; j < L; ++j)
      rk[(std::size_t)i * (std::size_t)L + (std::size_t)j] = perm[(std::size_t)j];
  }
  return rk;
}

// Runs fn and reports whether it threw std::runtime_error. A different exception
// type counts as a failure: the contract says these guards refuse specifically.
template <class F>
bool throws_runtime_error(F fn) {
  try {
    fn();
  } catch (const std::runtime_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

// Make row q NOT contain its own id, by replacing self with a FOREIGN id.
//
// Note what does NOT work: swapping self with another entry of the SAME row.
// That only RELOCATES self, and the core's self-pin searches the whole row, so
// nothing throws and the case silently tests nothing. Caught by the suite's own
// "did the fixture do what it claims" assertion (X6 in action: a check that the
// fixture is the thing we think it is, not just that the code under test
// returned something). Replacement has to come from outside the row.
//
// The row is no longer a permutation afterwards. That is intentional: this is a
// NEGATIVE case feeding deliberately invalid input to prove the guard fires.
std::vector<int> drop_self_from_row(const std::vector<int>& in, int q, int L, int N) {
  std::vector<int> out = in;
  std::vector<int> present;
  for (int j = 0; j < L; ++j) present.push_back(out[(std::size_t)L * q + (std::size_t)j]);
  int repl = -1;
  for (int cand = 0; cand < N && repl < 0; ++cand)
    if (std::find(present.begin(), present.end(), cand) == present.end()) repl = cand;
  for (int j = 0; j < L; ++j) {
    if (out[(std::size_t)L * q + (std::size_t)j] == q) {
      out[(std::size_t)L * q + (std::size_t)j] = repl;
      break;
    }
  }
  return out;
}

}  // namespace

int main() {
  std::printf("RFE corner suite -- contract refusals + multi-seed parity\n");

  const double pa = 0.1;

  // ---------------------------------------------------------------------
  // B) CONTRACT: constructor guards (RFECore.hpp:69-78)
  // ---------------------------------------------------------------------
  {
    const int N = 40, L = 10, k = 4, t = 2;
    std::vector<int> rk = gen_fixture(7, N, L);
    check(throws_runtime_error([&] {
            RFECore bad(rk, (size_t)N, (size_t)L, 1, t, pa);       // k < 2
          }),
          "constructor refuses k < 2 (rankKScore would be 0/0)");
    check(throws_runtime_error([&] {
            RFECore bad(rk, (size_t)N, (size_t)L, L + 1, t, pa);  // k > L
          }),
          "constructor refuses k > L (createEmb would read rkLists OOB)");
    check(throws_runtime_error([&] {
            RFECore bad(rk, (size_t)N, (size_t)L + 1, k, t, pa);  // L > N
          }),
          "constructor refuses L > N");
    std::vector<int> wrong = rk;
    wrong.pop_back();
    check(throws_runtime_error([&] {
            RFECore bad(wrong, (size_t)N, (size_t)L, k, t, pa);
          }),
          "constructor refuses rkLists whose size != N*L");
  }

  // ---------------------------------------------------------------------
  // B) CONTRACT: row without its own id (RFECore.hpp:1282-1284 and 777-780).
  // This is the premise the grValue>0 theorem needs, so it is that theorem's
  // regression guard. The legacy instead reads rkTmp[i] BEFORE testing the
  // bound (RFE.cpp:755, 816), i.e. an out-of-bounds read of the next row.
  // ---------------------------------------------------------------------
  {
    const int N2 = 12, L2 = 6, k2 = 3, t2 = 1;
    const std::vector<int> base = gen_fixture(11, N2, L2);
    const std::vector<int> rk_no_self = drop_self_from_row(base, 0, L2, N2);

    bool row0_lost_self = true;   // X6: confirm the fixture does what it claims
    for (int j = 0; j < L2; ++j)
      if (rk_no_self[(std::size_t)L2 * 0 + (std::size_t)j] == 0) row0_lost_self = false;
    check(row0_lost_self, "corner fixture really removed row 0's own id");

    // RFECore's constructor takes a non-const vector& because the legacy's sorts
    // mutate the rank lists in place. Each case therefore needs its OWN mutable
    // copy -- sharing one across the two cases would let the first case's partial
    // mutation leak into the second and make the second test something else.
    check(throws_runtime_error([&] {
            std::vector<int> m1 = rk_no_self;
            RFECore c(m1, (size_t)N2, (size_t)L2, k2, t2, pa);
            c.f1_rank_norm();
          }),
          "f1_rank_norm refuses a row with no self (legacy reads out of bounds)");

    check(throws_runtime_error([&] {
            std::vector<int> m2 = rk_no_self;
            RFECore c(m2, (size_t)N2, (size_t)L2, k2, t2, pa);
            c.f1_rank_norm();
            c.f2_create_emb();
            c.f3_create_reverse_emb();
            c.f4_combine_reverse_emb();
            c.f5_sort_emb();
            c.f5_effect_estimation_emb();
            c.f6_gram_and_inc_elems();
            c.f7b_rerank_cprr_tail();
          }),
          "the full chain up to f7b refuses a row with no self");
  }

  // ---------------------------------------------------------------------
  // A) PARITY: multi-seed full pipeline. Checklist item 4 wants several seeds;
  // each seed also varies `t`, so the t-loop length is covered too.
  // ---------------------------------------------------------------------
  {
    int seeds_ok = 0;
    for (int seed = 1; seed <= 5; ++seed) {
      const int Ns = 40, Ls = 10, ks = 4, ts = 1 + (seed % 2);
      const std::vector<int> rk = gen_fixture(seed, Ns, Ls);

      RFERef ref(Ns, Ls, ks, ts, pa);
      ref.load_rk(rk);
      ref.rank_norm();
      for (int ct = 0; ct < ts; ++ct) ref.exec_embedding_rr_pub();
      ref.exec_cartesian_product_pub();

      std::vector<int> rkc = rk;
      RFECore core(rkc, (size_t)Ns, (size_t)Ls, ks, ts, pa);
      core.f1_rank_norm();
      for (int ct = 0; ct < ts; ++ct) {
        core.f2_create_emb();
        core.f3_create_reverse_emb();
        core.f4_combine_reverse_emb();
        core.f7a_rerank_by_emb_sim();
      }
      core.f2_create_emb();
      core.f3_create_reverse_emb();
      core.f4_combine_reverse_emb();
      core.f5_sort_emb();
      core.f5_effect_estimation_emb();
      core.f6_gram_and_inc_elems();
      core.f7b_rerank_cprr_tail();

      long diff = 0;
      for (int q = 0; q < Ns; ++q)
        for (int i = 0; i < Ls; ++i)
          if (ref.rk()[(size_t)Ls * q + (size_t)i] !=
              core.rk_ref()[(size_t)Ls * q + (size_t)i]) ++diff;
      if (diff == 0) ++seeds_ok;
      else std::printf("    seed %d: %ld of %d rank cells differ\n", seed, diff, Ns * Ls);
    }
    check(seeds_ok == 5, "full-pipeline parity on 5 seeds (rank lists, not just values)");
  }

  // ---------------------------------------------------------------------
  // A) PARITY: tiny N=8, L=8, k=2 -- the most saturated shape the core accepts.
  // Gram compared cell by cell, with a non-vacuity assertion (X6).
  // ---------------------------------------------------------------------
  {
    const int Nt = 8, Lt = 8, kt = 2, tt = 1;
    const std::vector<int> rk = gen_fixture(3, Nt, Lt);
    RFERef ref(Nt, Lt, kt, tt, pa);
    ref.load_rk(rk);
    ref.rank_norm();
    ref.hyper_rr_embedding();
    ref.effec_estimation();
    ref.cprr_gram_only();

    std::vector<int> rkc = rk;
    RFECore core(rkc, (size_t)Nt, (size_t)Lt, kt, tt, pa);
    core.f1_rank_norm();
    core.f2_create_emb();
    core.f3_create_reverse_emb();
    core.f4_combine_reverse_emb();
    core.f5_sort_emb();
    core.f5_effect_estimation_emb();
    core.f6_gram_and_inc_elems();

    long cells = 0, bad = 0;
    for (int q = 0; q < Nt; ++q)
      for (const int j : ref.inc_elems()[(size_t)q]) {
        ++cells;
        const float lv = (q == j) ? (float)pow(2, 30) : ref.gram_at(q, j);
        const float cv = (q == j) ? (float)pow(2, 30) : core.gram().get(q, j);
        if (lv != cv) ++bad;
      }
    std::printf("  tiny Gram: compared %ld cells, %ld differ\n", cells, bad);
    check(cells > 0, "tiny Gram comparison was non-vacuous (X6)");
    check(bad == 0, "tiny Gram matches the legacy bit-exactly");
  }

  // ---------------------------------------------------------------------
  // C) RowAcc::finalize_candidates contract (Step 3 / Stage 3b, Camada 2)
  //
  // The O(k log k) rewrite is licensed by ONE fact: pass 1 carries no value,
  // so "distinct ascending columns with 0.0f values" is the answer under ANY
  // correct sort. Asserted here rather than left as a comment, because if it
  // ever stops being true the Gram changes silently instead of loudly.
  // ---------------------------------------------------------------------
  {
    // Deliberate multiset: 6 distinct columns {3,7,19,40,55,88}, repeated unevenly
    // and delivered out of order. raw/uniq was measured at 5.36 on real ALOI,
    // so heavy duplication is the real shape, not an artificial edge case.
    const int distinct[] = {40, 7, 19, 3, 88, 7, 40, 19, 3, 55, 7, 88, 19, 40, 3};
    const std::vector<int> raw(std::begin(distinct), std::end(distinct));
    std::vector<int> cols_out;
    std::vector<float> vals_out;
    RFECore::probe_candidates(raw, cols_out, vals_out);

    bool ascending = true, all_zero = true, sizes_match = true;
    for (std::size_t i = 1; i < cols_out.size(); ++i)
      if (!(cols_out[i - 1] < cols_out[i])) ascending = false;   // strict: distinct
    for (const float v : vals_out) if (v != 0.0f) all_zero = false;
    if (cols_out.size() != vals_out.size()) sizes_match = false;

    check(ascending,
          "finalize_candidates yields DISTINCT ASCENDING columns (V4 fast-path contract)");
    check(all_zero,
          "finalize_candidates yields bit-exactly 0.0f per column (no value carried)");
    check(sizes_match, "finalize_candidates sizes cols and vals identically");
    check(cols_out.size() == 6,
          "finalize_candidates collapsed the 15-element multiset to 6 distinct columns");
    check(cols_out.size() == vals_out.size() && !cols_out.empty(),
          "finalize_candidates comparison was non-vacuous (X6)");

    std::vector<int> e_cols; std::vector<float> e_vals;
    RFECore::probe_candidates({}, e_cols, e_vals);
    check(e_cols.empty() && e_vals.empty(), "finalize_candidates handles an empty row");

    std::vector<int> s_cols; std::vector<float> s_vals;
    RFECore::probe_candidates({77}, s_cols, s_vals);
    check(s_cols.size() == 1 && s_cols[0] == 77 && s_vals.size() == 1 &&
              s_vals[0] == 0.0f,
          "finalize_candidates handles a single candidate");

    // All-equal input: the worst case for a stable sort and the case where an
    // order-sensitive coalescing would be most visible.
    std::vector<int> same(200, 5);
    std::vector<int> a_cols; std::vector<float> a_vals;
    RFECore::probe_candidates(same, a_cols, a_vals);
    check(a_cols.size() == 1 && a_cols[0] == 5 && a_vals.size() == 1 && a_vals[0] == 0.0f,
          "finalize_candidates collapses an all-equal row to one 0.0f column");
  }

  std::printf("\nRFE CORNER: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}