// test_rfe_golden.cpp
//
// Step 0 of the RFE port: the golden, recorded BY THE VERBATIM LEGACY CORE.
//
// This file runs rfe_ref_verbatim.hpp (the repo-root RFE.cpp with only
// framework glue shimmed) and records what it produces. It is the reference the
// CSR port is validated against in Step 2, and the numbers the Step 1 hypothesis
// gate is derived from. It asserts NOTHING about the port -- the only thing it
// pins is the legacy's own output, plus structural invariants that must hold for
// ANY input (self at position 0, rows stay permutations).
//
// WHY A STRONG CRITERION AND NOT A TRANSCRIPT MD5 (briefing rule 3)
//   md5 of a pass/fail suite only attests pass/fail; it proves neither value
//   equality nor order. So each phase emits THREE separate instruments:
//     * FNV-1a over the raw bytes of rkLists            (did the ORDER move)
//     * FNV-1a over the raw bytes of the matrix cells  (did the VALUES move)
//     * nnz + the structural census                    (did the SUPPORT move)
//   The phase digests go to a file so an md5 of that file is a third, redundant
//   check. The census is the part that catches the plan's central structural
//   claim: that emb carries keys whose value is EXACTLY zero, and that those
//   keys are load-bearing (they reach incElems and therefore the output rank
//   lists). If that count is 0 on this fixture the claim is untested here and
//   the fixture must be strengthened -- the file says so in the output rather
//   than passing silently.
//
// THE CENSUS NUMBERS THIS STEP IS EXPECTED TO ESTABLISH (all are predictions
// from reading RFE.cpp, to be confirmed here, not assumed):
//   * emb_exact_zero_keys() > 0. rankKScore(k) == 1 - log(k)/log(k) == 0.0f
//     exactly (RFE.cpp:645), and incEmb INSERTS the key (RFE.cpp:692), so
//     pos1 = k-1 writes structural zeros.
//   * emb_keys_without_reverse() > 0. emb is strongly asymmetric, so
//     combineReverseEmb multiplies many entries by an operator[]-inserted 0.0f
//     while KEEPING the key (RFE.cpp:639).
//   * inc_elems_total() > n*l. The Gram DISCOVERS candidates outside the rank
//     lists; that is the candidate-discovery pattern the CSR port must serve
//     with pruning rather than accumulation.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -I Source/tests (one line)
//       Source/tests/test_rfe_golden.cpp -o /tmp/test_rfe_golden
// Run: /tmp/test_rfe_golden            (records the golden; exit 0 = invariants)
//      /tmp/test_rfe_golden --record   (also writes the golden to stdout)
//
// Posture: -O2, no fast-math, -ffp-contract=off. The pins below are an -O2
// contract; whether they survive the product flags is Step 4's measurement, not
// assumed here (see rfe_ref_verbatim.hpp's build-posture note).

#include "rfe_ref_verbatim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
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

// Fixture: row i = a permutation of [0,N) with i at index 0. Same generator
// shape as bench_bfstree.cpp gen_rklists / test_bfstree_parity.cpp (mt19937 +
// shuffle + self moved to index 0), so a rank list is always a permutation with
// self first. That self-at-0 is a PRECONDITION of the legacy here: both sorts
// read element i before testing i < bound (RFE.cpp:755, 816).
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

// Canonical per-phase record. One line per phase, all instruments on it, so a
// single md5 of the golden file is a redundant third check and a diff between
// two runs localises the phase that moved.
struct PhaseRec {
  // std::string, not const char*: an earlier version stored a pointer to a
  // stack-local buffer in the embRR loop and the recorded phase names came out
  // as garbage ("R#a"). A golden record has to be stable, and the fix belongs
  // in the record type rather than in the caller.
  std::string name;
  std::uint64_t ck_rk;     // FNV over raw bytes of rkLists
  std::uint64_t ck_matrix; // FNV over raw bytes of the n*n matrix
  std::size_t nnz;         // nonzero matrix cells
  std::size_t emb_keys;    // census (only meaningful once emb exists)
  std::size_t emb_zeros;   //   ... of which value == 0.0f exactly
  std::size_t emb_norev;   //   ... of which have no reverse partner
  std::size_t inc_total;   // |incElems| total
};

void snap(const RFERef& r, const std::string& name, PhaseRec& out) {
  out.name = name;
  const std::vector<int>& rk = r.rk();
  out.ck_rk = rfe_fnv1a(rk.data(), rk.size() * sizeof(int));
  const std::vector<float>& m = r.matrix();
  out.ck_matrix = rfe_fnv1a(m.data(), m.size() * sizeof(float));
  out.nnz = r.matrix_nnz();
  out.emb_keys = r.emb_total_keys();
  out.emb_zeros = r.emb_exact_zero_keys();
  out.emb_norev = r.emb_keys_without_reverse();
  out.inc_total = r.inc_elems_total();
}

void print_rec(const PhaseRec& p) {
  // .c_str(), not p.name: printf("%s", std::string) is undefined behaviour and
  // reads the object representation as a pointer. It appeared here as a
  // non-deterministic md5 (the printed address changed run to run).
  std::printf("%-22s ck_rk=0x%016llx ck_matrix=0x%016llx nnz=%zu "
              "emb_keys=%zu emb_zeros=%zu emb_norev=%zu inc_total=%zu\n",
              p.name.c_str(), (unsigned long long)p.ck_rk, (unsigned long long)p.ck_matrix,
              p.nnz, p.emb_keys, p.emb_zeros, p.emb_norev, p.inc_total);
}

// Structural invariants that must hold for ANY input. Cheap, and they catch the
// classes of bug that a digest alone would report only as "some number moved".
//
// PHASE-AWARE, and this is a correction the first run of this file forced. The
// permutation invariant is TRUE after rankNorm and after each embedding RR --
// only the legacy's own execSortRankedLists ran, which reorders without changing
// the row's membership. It is FALSE, and MUST be false, after the Cartesian
// product: kernelSortRankedLists_extra replaces row q with the top-L of
// (current row UNION incElems[q]), so the output row legitimately contains ids
// the query never listed. That is the candidate-discovery contract, and an
// earlier version of this check asserted "permutation" everywhere and FAILED --
// the legacy was right and the check was wrong.
//
// The correct final-phase invariant is therefore a CONTAINMENT, plus a
// positivity claim:
//   (a) every id of the final row q is in (input row q) UNION incElems[q];
//   (b) at least one row gained an id it did not have (discovery happened);
//   (c) no id is out of [0,N).
void check_invariants_perm(const std::vector<int>& rk_in, const std::vector<int>& rk_out,
                           int N, int L) {
  bool self0 = true, perm = true, inrange = true;
  for (int i = 0; i < N; i++) {
    if (rk_out[(std::size_t)i * (std::size_t)L] != i) self0 = false;
    std::vector<int> a(rk_in.begin() + (size_t)i * (size_t)L,
                       rk_in.begin() + (size_t)(i + 1) * (size_t)L);
    std::vector<int> b(rk_out.begin() + (size_t)i * (size_t)L,
                       rk_out.begin() + (size_t)(i + 1) * (size_t)L);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) perm = false;
    for (int v : b)
      if (v < 0 || v >= N) inrange = false;
  }
  expect(self0, "self at position 0 in every output row (sort pin)");
  expect(perm, "output row is a permutation of its input row (pre-CPRR phases)");
  expect(inrange, "every id in every output row is in [0,N)");
}

void check_invariants_final(const std::vector<int>& rk_in, const std::vector<int>& rk_out,
                            const RFERef& ref, int N, int L) {
  bool inrange = true, contained = true, dupfree = true;
  std::size_t gained_total = 0;
  for (int q = 0; q < N; q++) {
    std::vector<int> before(rk_in.begin() + (size_t)q * (size_t)L,
                            rk_in.begin() + (size_t)(q + 1) * (size_t)L);
    std::sort(before.begin(), before.end());
    const std::set<int>& cand = ref.inc_elems()[(std::size_t)q];

    std::vector<int> row(rk_out.begin() + (size_t)q * (size_t)L,
                         rk_out.begin() + (size_t)(q + 1) * (size_t)L);
    std::vector<int> sorted_row = row;
    std::sort(sorted_row.begin(), sorted_row.end());
    if (std::adjacent_find(sorted_row.begin(), sorted_row.end()) != sorted_row.end())
      dupfree = false;

    for (int v : row) {
      if (v < 0 || v >= N) inrange = false;
      const bool was_listed = std::binary_search(before.begin(), before.end(), v);
      const bool was_cand = cand.count(v) != 0;
      if (!was_listed && !was_cand) contained = false;
      if (!was_listed) ++gained_total;
    }
  }
  expect(inrange, "final: every id in [0,N)");
  expect(dupfree, "final: no duplicate id inside a row");
  expect(contained,
         "final: every id is in (input row UNION incElems[q]) -- nothing invented");
  expect(gained_total > 0,
         "final: rows gained ids the query never listed (discovery is real)");
  std::printf("  info: %zu of %d final row slots are newly discovered candidates\n",
              gained_total, N * L);
}

}  // namespace

int main(int argc, char** argv) {
  // Arg convention: --record is a flag; the positional form is N L k t seed.
  // "record" is detected by being exactly the literal --record, so it is never
  // mistaken for the N argument.
  const bool record = (argc > 1 && std::strcmp(argv[1], "--record") == 0);
  const int off = record ? 2 : 1;   // argv offset of the positional fixture args

  // Fixture. Overridable from argv so the sweep can vary N/L/k/t without
  // recompiling. N is small on purpose: the legacy is O(N^2) dense AND does
  // O(N*k^2) std::map insertions per createEmb, which runs t+1 times. Same
  // regime (and same reason) as the BFSTree golden's N=800.
  //   argv: N L k t seed   (defaults are the PINNED golden fixture)
  const int N    = (argc > off + 0) ? std::atoi(argv[off + 0]) : 200;
  const int L    = (argc > off + 1) ? std::atoi(argv[off + 1]) : 40;
  const int k    = (argc > off + 2) ? std::atoi(argv[off + 2]) : 10;
  const int t    = (argc > off + 3) ? std::atoi(argv[off + 3]) : 2;
  const int seed = (argc > off + 4) ? std::atoi(argv[off + 4]) : 42;
  const double pa = 0.1;

  std::printf("RFE golden (verbatim legacy core) -- Step 0\n");
  std::printf("fixture N=%d L=%d k=%d t=%d pa=%.3f seed=%d\n", N, L, k, t, pa, seed);
  std::printf("(self pinned at position 0; rows are permutations)\n");

  const std::vector<int> rk_in = gen_fixture(seed, N, L);

  RFERef ref(N, L, k, t, pa);
  ref.load_rk(rk_in);

  // Phase-by-phase, calling the legacy's phases individually. Nothing is fused
  // and nothing is skipped: the digests below are only comparable to a port that
  // runs the same phase boundaries.
  std::vector<PhaseRec> recs;
  PhaseRec p;

  ref.rank_norm();                       // rankNorm + execSortRankedLists
  snap(ref, "rankNorm", p);
  recs.push_back(p);
  print_rec(p);
  check_invariants_perm(rk_in, ref.rk(), N, L);   // reorder only, so far

  for (int ct = 0; ct < t; ct++) {
    ref.embedding_rr();                  // createEmb/createReverseEmb/
                                         // combineReverseEmb + reRankingByEmbSim
    char nm[64];
    std::snprintf(nm, sizeof(nm), "embRR[%d]", ct);
    snap(ref, nm, p);
    recs.push_back(p);
    print_rec(p);
    check_invariants_perm(rk_in, ref.rk(), N, L);
  }

  // After the embedding RR the census is meaningful; assert the plan's central
  // structural claims on it. These are ASSERTIONS ABOUT THE LEGACY, so if one
  // is false the plan was wrong and the port's design has to change -- that is
  // exactly what Step 0 is for.
  // The census assertions, scoped by REGIME.
  //
  // The plan predicted `emb` carries keys whose value is EXACTLY 0.0f, and that
  // is CONFIRMED at the pinned fixture (7910 of 12812 keys). It is NOT true for
  // every (N,L,k): step0_probe_k.sh measured the zero FRACTION falling 76% ->
  // 0.05% as k goes 3 -> 40 at N=200 L=40, because the support saturates
  // (|supp| 3% -> 100% of n^2) and `acc += 0.0f` stops being observable once
  // other pos1 terms have written the same column.
  //
  // So the honest assertion is on the MECHANISM and the REGIME, not on a count:
  //   - saturation is reported, never assumed;
  //   - the exact-zero and no-reverse-partner claims are asserted ONLY when the
  //     fixture is in the sparse regime, and SKIPPED (loudly) when saturated,
  //     because a saturated fixture cannot test them.
  // Asserting them unconditionally is what made the k=40/60/100 sweep rows fail
  // -- the legacy was right and the check was over-specified.
  const double sat = ref.emb_saturation();
  const bool sparse = (sat < 0.90);
  std::printf("  info: emb saturation |supp|/n^2 = %.4f (%s regime)\n", sat,
              sparse ? "SPARSE" : "SATURATED");

  expect(ref.emb_total_keys() > 0, "emb is non-empty");
  expect(ref.emb_negative_keys() == 0, "no negative emb values (all inputs >= 0)");

  if (sparse) {
    expect(ref.emb_exact_zero_keys() > 0,
           "[sparse] emb carries keys with EXACTLY 0.0f (rankKScore(K)==0)");
    expect(ref.emb_keys_without_reverse() > 0,
           "[sparse] emb has keys with no reverse partner (combineReverseEmb)");
  } else {
    // Not a pass and not a failure: the fixture is in a regime where these two
    // populations are absorbed, so it cannot test them. Step 3 on real data is
    // what settles the production-regime question.
    std::printf("  SKIP: exact-zero / no-reverse-partner not asserted -- fixture is "
                "SATURATED (sat=%.4f), a Step 3 real-data question\n", sat);
  }

  ref.cartesian_product();               // doHyperRREmbedding + doHyperEffec
                                         // Estimation + reRankingByCPRR
  snap(ref, "cartesianProduct", p);
  recs.push_back(p);
  print_rec(p);

  // The candidate-discovery claim: the Gram discovered entries OUTSIDE the
  // rank lists. |incElems| counts (i,j) pairs touched by the Gram; the rank
  // lists hold n*l distinct (i,j) at most. inc_total > n*l means the Gram
  // found candidates the query never listed.
  std::printf("\n-- candidate discovery --\n");
  expect(ref.inc_elems_total() > (std::size_t)N * (std::size_t)L,
         "Gram discovered candidates beyond the rank lists (inc_total > N*L)");
  expect(ref.inc_elems_max_row() > (std::size_t)L,
         "some row has more candidates than L (so top-L pruning is required)");

  // After the Cartesian product the permutation invariant NO LONGER HOLDS, by
  // design: the row is now the top-L of (row UNION discovered candidates). The
  // containment invariant below is the correct replacement.
  check_invariants_final(rk_in, ref.rk(), ref, N, L);

  if (record) {
    std::printf("\n-- golden (canonical; md5 this block) --\n");
    for (const PhaseRec& q : recs) print_rec(q);
    std::printf("FINAL_CK_RK=0x%016llx\n", (unsigned long long)recs.back().ck_rk);
  }

  if (failures == 0) {
    std::printf("RFE GOLDEN: OK\n");
    return 0;
  }
  std::printf("RFE GOLDEN: %d FAILURE(S)\n", failures);
  return 1;
}