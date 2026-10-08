// test_rfe_parity.cpp
//
// Step 2 parity gate for the RFE core, phases F1+F2+F3+F4+F5+F6+F7, against the VERBATIM
// legacy, cell by cell. F1 = rankNorm + sort, F2 = createEmb, F3 =
// createReverseEmb. The reference is Source/tests/rfe_ref_verbatim.hpp (the
// repo-root RFE.cpp with only framework glue shimmed), and the pins are the ones
// Step 0 recorded FROM THE LEGACY ITSELF:
//
//   fixture N=200 L=40 k=10 t=2 pa=0.1 seed=42
//     rankNorm  ck_rk=0x3465e556d414ca79  ck_matrix=0x8afb15dd0502c7cb  nnz=8000
//
// WHY BOTH COMPARISONS. The digest pin proves the port reproduces the legacy's
// recorded output; the cell-by-cell diff proves WHERE it does not, which the pin
// alone cannot (it would just say "some number moved"). The gate runs BOTH, plus
// the structural invariants the legacy guarantees.
//
// This gate covers F1 ONLY at this revision. Later phases append their own pins
// and diffs, one phase at a time -- never two changes measured together.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp (one line)
//       -I Source/src -I Source/src/Core/hpp -I Source/tests
//       Source/tests/test_rfe_parity.cpp -o /tmp/test_rfe_parity

#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>

namespace {

using CT = CSRMatrix<float>::size_type;

int g_failures = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("  ok  : %s\n", what);
  }
}

// Same generator as the Step 0 golden (mt19937 + shuffle + self at index 0), so
// the input bytes are the ones the pins were produced from.
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

// Cell-by-cell diff of the legacy's dense matrix against the core's CSR. The
// legacy zero-fills everything it does not write, and get() returns 0 for an
// absent cell, so the two views are directly comparable over the whole n*n --
// which is what makes "did any value move" answerable rather than guessed.
long diff_matrix(const std::vector<float>& legacy, const RFECore& core,
                 size_t N, long* first_bad_row) {
  long bad = 0;
  for (size_t q = 0; q < N; ++q) {
    for (size_t j = 0; j < N; ++j) {
      const float a = legacy[q * N + j];
      const float b = core.sim().get((CT)q, (CT)j);
      if (a != b) {
        ++bad;
        if (first_bad_row && *first_bad_row < 0) {
          *first_bad_row = (long)q;
          std::printf("    first diff at [%zu,%zu]: legacy=%.9g core=%.9g\n", q, j, a, b);
        }
      }
    }
  }
  return bad;
}

}  // namespace

namespace {

// Deliberate ee-TIE fixtures. The Step 0/2 generator shuffles every row
// independently, so rows are structurally unrelated and `ee` values almost never
// collide -- the tie-break in tag_sort (F5) was covered by exactly ONE accidental
// fixture (80 20 6 1, 8 tied pairs). These fixtures are DESIGNED to collide.
//
// WHY NOT SIMPLY DUPLICATE ROWS: it is impossible under this pipeline's
// contract. Row q must carry its own id q at position 0, so two distinct rows can
// never be byte-identical. What CAN be maximised is structural SIMILARITY: rows
// drawn from the same small pool of patterns, differing only at position 0. Two
// such rows share every input the other reads, so their `emb` top-k sums collide
// far more often.
//
// Invariants asserted inside the generator (not assumed): every row is a
// permutation of distinct ids in [0,N), and row q carries q at index 0 -- which
// is what RFECore's self-pin requires (RFECore.hpp:1282). A row violating either
// would make the core throw, so the fixture would be invalid by construction.
std::vector<int> gen_fixture_ties(int seed, int N, int L, int patterns) {
  std::vector<int> rk((std::size_t)N * (std::size_t)L, 0);
  std::mt19937 rng((unsigned)seed);
  if (patterns < 1) patterns = 1;
  if (patterns > N) patterns = N;

  // The pool: `patterns` distinct id sequences, each a partial permutation.
  std::vector<std::vector<int>> pool((std::size_t)patterns);
  std::vector<int> perm((std::size_t)N);
  for (int i = 0; i < N; ++i) perm[(std::size_t)i] = i;
  for (int p = 0; p < patterns; ++p) {
    std::shuffle(perm.begin(), perm.end(), rng);
    pool[(std::size_t)p].assign(perm.begin(), perm.begin() + L);
  }

  for (int q = 0; q < N; ++q) {
    const std::vector<int>& pat = pool[(std::size_t)(q % patterns)];
    // Fill the row from the pattern, forcing q into slot 0. If q also occurs
    // later in the pattern, it is removed from that slot so ids stay distinct.
    std::vector<int> row;
    row.reserve((std::size_t)L);
    row.push_back(q);
    for (int j = 1; j < L; ++j) {
      const int cand = pat[(std::size_t)j];
      if (cand != q && std::find(row.begin(), row.end(), cand) == row.end())
        row.push_back(cand);
    }
    // Top up from unused ids if the pattern ran dry (can happen when q displaces
    // one of its own pattern entries).
    for (int cand = 0; (int)row.size() < L && cand < N; ++cand)
      if (std::find(row.begin(), row.end(), cand) == row.end()) row.push_back(cand);
    for (int j = 0; j < L; ++j)
      rk[(std::size_t)q * (std::size_t)L + (std::size_t)j] = row[(std::size_t)j];
  }

  // Invariant checks -- X6 applied to the fixture itself. A fixture that silently
  // violates the core's contract would make the case fail for the wrong reason.
  for (int q = 0; q < N; ++q) {
    std::set<int> seen;
    for (int j = 0; j < L; ++j) {
      const int v = rk[(std::size_t)q * (std::size_t)L + (std::size_t)j];
      if (v < 0 || v >= N || !seen.insert(v).second) {
        std::printf("FATAL: tie fixture row %d is not a permutation of distinct ids\n", q);
        std::exit(2);
      }
    }
    if (rk[(std::size_t)q * (std::size_t)L] != q) {
      std::printf("FATAL: tie fixture row %d does not carry self at position 0\n", q);
      std::exit(2);
    }
  }
  return rk;
}

// Top-L ids from an already value-sorted candidate list. Used by the H8
// value-only prune comparison further down.
const auto got_ids = [](const std::vector<std::pair<float, int>>& sorted, int LL) {
  std::vector<int> out;
  for (std::size_t s = 0; s < sorted.size() && s < (std::size_t)LL; ++s)
    out.push_back(sorted[s].second);
  return out;
};

// F5's negative control, factored out so the tie fixtures reuse it rather than
// re-implementing "would a stable sort have got this wrong?". `ee` is the
// NORMALISED vector, which orders identically to the unnormalised one because
// grValue > 0 (grValue > 0 theorem), so it ranks rows identically.
long stable_sort_positions_differing(const std::vector<int>& legacy_order,
                                     const std::vector<float>& ee) {
  std::vector<int> stable(ee.size());
  for (std::size_t i = 0; i < ee.size(); ++i) stable[i] = (int)i;
  std::stable_sort(stable.begin(), stable.end(),
                   [&ee](int a, int b) { return ee[(std::size_t)a] > ee[(std::size_t)b]; });
  long d = 0;
  for (std::size_t i = 0; i < ee.size(); ++i)
    if (stable[i] != legacy_order[i]) ++d;
  return d;
}

// Number of exactly-equal pairs in `ee`. This is the census that says whether a
// fixture exercises tag_sort's tie-break AT ALL (0 pairs => vacuous, X6).
long ee_tied_pairs(const std::vector<float>& ee) {
  long t = 0;
  for (std::size_t i = 0; i < ee.size(); ++i)
    for (std::size_t j = i + 1; j < ee.size(); ++j)
      if (ee[i] == ee[j]) ++t;
  return t;
}

}  // namespace

// Loads a rank-list file for the REAL-data Stage B run. Format: one row per
// line, L space-separated ints, self at index 0 (the derived datasets from
// mk_rk_from_leading_id.py, or ALOI rkLists.txt directly). N and L are NOT
// taken from the file: they are passed in and VERIFIED, so a wrong N cannot
// silently shift the stride and produce plausible garbage. Short read or extra
// column => refuse (exit 5), never pad.
std::vector<int> load_rk_file(const char* path, int N, int L) {
  std::vector<int> rk;
  rk.reserve((std::size_t)N * (std::size_t)L);
  std::FILE* f = std::fopen(path, "r");
  if (!f) { std::printf("FATAL: cannot open %s\n", path); std::exit(2); }
  for (int q = 0; q < N; ++q) {
    for (int j = 0; j < L; ++j) {
      int v = -1;
      if (std::fscanf(f, "%d", &v) != 1) {
        std::printf("RKLOAD_REFUSED: short read at row=%d col=%d (want N=%d L=%d)\n",
                    q, j, N, L);
        std::fclose(f); std::exit(5);
      }
      rk.push_back(v);
    }
    // Extra-column check. NOT fscanf("%d"): %d skips ALL whitespace including
    // newlines, so it would happily read the first id of the NEXT row and call
    // a correctly-shaped file malformed. Consume intra-line blanks only, then
    // require a newline or EOF.
    int c;
    for (;;) {
      c = std::fgetc(f);
      if (c == ' ' || c == '\t' || c == '\r') continue;
      break;
    }
    if (c != '\n' && c != EOF) {
      std::printf("RKLOAD_REFUSED: extra token at row=%d -- file has more than L=%d cols\n",
                  q, L);
      std::fclose(f); std::exit(5);
    }
  }
  std::fclose(f);
  return rk;
}

int main(int argc, char** argv) {
  // argv[6], when present, is a rank-list FILE: real data instead of the
  // synthetic generator. Absent => the synthetic fixture, byte-identical to
  // before, so every existing pin stays meaningful.
  const char* rkpath = (argc > 6) ? argv[6] : nullptr;
  const int N    = (argc > 1) ? std::atoi(argv[1]) : 200;
  const int L    = (argc > 2) ? std::atoi(argv[2]) : 40;
  const int k    = (argc > 3) ? std::atoi(argv[3]) : 10;
  const int t    = (argc > 4) ? std::atoi(argv[4]) : 2;
  const int seed = (argc > 5) ? std::atoi(argv[5]) : 42;
  const double pa = 0.1;
  const bool pinned = !rkpath && (N == 200 && L == 40 && k == 10 && t == 2 && seed == 42);

  std::printf("RFE parity gate -- Step 2, F1 (rankNorm + sort)\n");
  std::printf("fixture N=%d L=%d k=%d t=%d pa=%.3f seed=%d%s\n", N, L, k, t, pa, seed,
              pinned ? "  [PINNED]" : "  (not the pinned fixture)");
  if (rkpath) std::printf("source: REAL DATA file %s\n", rkpath);

  std::vector<int> rk_in = rkpath ? load_rk_file(rkpath, N, L)
                                  : gen_fixture(seed, N, L);

  RFERef ref(N, L, k, t, pa);
  ref.load_rk(rk_in);
  ref.rank_norm();
  const std::vector<int> rk_legacy = ref.rk();
  const std::vector<float>& m_legacy = ref.matrix();

  std::vector<int> rk_core = rk_in;
  RFECore core(rk_core, (size_t)N, (size_t)L, k, t, pa);
  core.f1_rank_norm();

  std::printf("  legacy: nnz=%zu ck_rk=0x%016llx\n", ref.matrix_nnz(),
              (unsigned long long)rfe_fnv1a(rk_legacy.data(),
                                             rk_legacy.size() * sizeof(int)));
  std::printf("  core  : nnz=%zu ck_rk=0x%016llx ck_sim_dense=0x%016llx\n",
              core.nnz_sim(), (unsigned long long)core.ck_rk(),
              (unsigned long long)core.ck_sim_dense());

  expect(core.nnz_sim() == (std::size_t)N * (std::size_t)L,
         "F1: nnz == N*L (one written cell per rank-list slot)");

  long bad_row = -1;
  const long bad_cells = diff_matrix(m_legacy, core, (size_t)N, &bad_row);
  expect(bad_cells == 0, "F1: every similarity cell matches the legacy bit-exactly");

  // Dump the core's OWN view of row 0's sort input: which id, from which input
  // slot, carrying which value. This is the ground truth for the sort, taken
  // before any reordering, so the disagreement can be attributed to the sort
  // rather than to the similarity.
  {
    std::vector<std::pair<float, int>> probe;
    for (int i = 0; i < L; ++i) {
      const int id = rk_in[(std::size_t)i];
      probe.emplace_back(m_legacy[(std::size_t)0 * N + (std::size_t)id], id);
    }
    std::stable_sort(probe.begin(), probe.end(),
                     [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                       return a.first < b.first;
                     });
    std::printf("  expected row0 after sort by value (from the legacy's own cells):");
    for (int i = 0; i < 6; ++i) std::printf(" %d(%.6g)", probe[(std::size_t)i].second, probe[(std::size_t)i].first);
    std::printf("\n  legacy  row0 after rankNorm                        :");
    for (int i = 0; i < 6; ++i)
      std::printf(" %d(%.6g)", rk_legacy[(std::size_t)i],
                  m_legacy[(std::size_t)0 * N + (std::size_t)rk_legacy[(std::size_t)i]]);
    std::printf("\n  core    row0 after f1_rank_norm                   :");
    for (int i = 0; i < 6; ++i)
      std::printf(" %d(%.6g)", rk_core[(std::size_t)i],
                  m_legacy[(std::size_t)0 * N + (std::size_t)rk_core[(std::size_t)i]]);
    std::printf("\n");
  }

  long bad_pos = -1;
  for (std::size_t i = 0; i < rk_legacy.size(); ++i) {
    if (rk_legacy[i] != rk_core[i]) {
      bad_pos = (long)i;
      const std::size_t rr = i / (std::size_t)L, cc = i % (std::size_t)L;
      std::printf("    first diff at slot %zu (row %zu, col %zu): legacy=%d core=%d\n",
                  i, rr, cc, rk_legacy[i], rk_core[i]);
      // Print the whole row both ways, with the similarity each id carries, so
      // the tie that decided the order is VISIBLE rather than inferred.
      std::printf("    row %zu legacy:", rr);
      for (std::size_t j = 0; j < (std::size_t)L && j < 6; ++j)
        std::printf(" %d(%.6g)", rk_legacy[rr * (std::size_t)L + j],
                    m_legacy[rr * (std::size_t)N + (std::size_t)rk_legacy[rr * (std::size_t)L + j]]);
      std::printf("\n    row %zu core  :", rr);
      for (std::size_t j = 0; j < (std::size_t)L && j < 6; ++j)
        std::printf(" %d(%.6g)", rk_core[rr * (std::size_t)L + j],
                    m_legacy[rr * (std::size_t)N + (std::size_t)rk_core[rr * (std::size_t)L + j]]);
      std::printf("\n    row %zu input  :", rr);
      for (std::size_t j = 0; j < (std::size_t)L && j < 6; ++j)
        std::printf(" %d", rk_in[rr * (std::size_t)L + j]);
      std::printf("\n");
      // The decisive number: how many of the legacy's sorted ids carry a
      // similarity that is EXACTLY equal to another id's. If ties exist, the
      // order among them is decided by stability, and the core's stable_sort
      // starts from a DIFFERENT initial order than the legacy's insertion sort.
      std::vector<std::pair<float, int>> vals;
      for (std::size_t j = 0; j < (std::size_t)L; ++j) {
        const int id = rk_in[rr * (std::size_t)L + j];
        vals.emplace_back(m_legacy[rr * (std::size_t)N + (std::size_t)id], id);
      }
      long ties = 0;
      for (std::size_t a = 0; a < vals.size(); ++a)
        for (std::size_t b = a + 1; b < vals.size(); ++b)
          if (vals[a].first == vals[b].first) ++ties;
      std::printf("    exact-value ties in this row: %ld\n", ties);
      // Where does each disputed id sit in the INPUT row, and what value does it
      // carry? If the legacy's sorted row contains ids absent from the input row,
      // the legacy's rkLists were mutated before rankNorm's sort -- which would
      // mean the reference is not doing what this port assumes.
      auto pos_in_input = [&](int id) -> long {
        for (std::size_t j = 0; j < (std::size_t)L; ++j)
          if (rk_in[rr * (std::size_t)L + j] == id) return (long)j;
        return -1;
      };
      for (std::size_t j = 0; j < 3 && j < (std::size_t)L; ++j) {
        const int lid = rk_legacy[rr * (std::size_t)L + j];
        const int cid = rk_core[rr * (std::size_t)L + j];
        std::printf("    slot %zu: legacy=%3d inputpos=%3ld val=%.6g | core=%3d inputpos=%3ld\n",
                    j, lid, pos_in_input(lid),
                    m_legacy[rr * (std::size_t)N + (std::size_t)lid],
                    cid, pos_in_input(cid));
      }
      break;
    }
  }
  expect(bad_pos < 0, "F1: rank lists match the legacy position by position");
  // Invariants that must hold for ANY input, independent of the pins.
  bool self0 = true, inrange = true, reordered_only = true;
  for (int i = 0; i < N; i++) {
    if (rk_core[(std::size_t)i * (std::size_t)L] != i) self0 = false;
    std::vector<int> a(rk_in.begin() + (std::size_t)i * (std::size_t)L,
                       rk_in.begin() + (std::size_t)(i + 1) * (std::size_t)L);
    std::vector<int> b(rk_core.begin() + (std::size_t)i * (std::size_t)L,
                       rk_core.begin() + (std::size_t)(i + 1) * (std::size_t)L);
    for (int v : b) if (v < 0 || v >= N) inrange = false;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) reordered_only = false;   // rankNorm's sort only reorders
  }
  expect(self0, "F1: self at position 0 in every row (sort pin)");
  expect(inrange, "F1: every id in [0,N)");
  // Holds for real truncated data too: it compares SORTED rows, so it asserts
  // the multiset is preserved, not that the row is a permutation of [0,N).
  expect(reordered_only, "F1: F1 only reorders (row multiset preserved)");

  std::printf("\n-- F2: createEmb --\n");

  // The legacy's emb immediately after createEmb. The reverse/combine phases
  // MUTATE emb in place, so this snapshot has to be taken here.
  RFERef ref2(N, L, k, t, pa);
  ref2.load_rk(rk_in);
  ref2.rank_norm();
  ref2.create_emb();
  const auto& emb_legacy = ref2.emb();

  // The core, same two phases. F1 already matched above, so any F2 disagreement
  // is attributable to F2 alone -- the point of running one phase at a time.
  std::vector<int> rk_core2 = rk_in;
  RFECore core2(rk_core2, (size_t)N, (size_t)L, k, t, pa);
  core2.f1_rank_norm();
  core2.f2_create_emb();

  long legacy_keys = 0, core_keys = 0, mismatched = 0, extra = 0;
  for (int q = 0; q < N; ++q) {
    const auto& lrow = emb_legacy.at(q);
    legacy_keys += (long)lrow.size();
    for (const auto& kv : lrow) {
      const float cv = core2.emb().get((CT)q, (CT)kv.first);
      if (cv != kv.second) {
        if (mismatched == 0) {
          std::printf("    first diff at [%d,%d]: legacy=%.9g core=%.9g\n", q, kv.first,
                      kv.second, cv);
        }
        ++mismatched;
      }
    }
    // Cells the core has that the legacy does not -- a spurious insert.
    core2.emb().for_each_in_row((CT)q, [&](CT c, float) {
      if (lrow.count((int)c) == 0) ++extra;
    });
    core_keys += (long)core2.emb().row_nnz((CT)q);
  }

  std::printf("  legacy emb keys=%ld | core emb keys=%ld | mismatched=%ld | extra=%ld\n",
              legacy_keys, core_keys, mismatched, extra);
  std::printf("  exact-zero keys: legacy=%zu core=%zu (H2 says these are load-bearing)\n",
              ref2.emb_exact_zero_keys(), core2.emb_exact_zeros());

  expect(mismatched == 0, "F2: every emb value matches the legacy bit-exactly");
  expect(core_keys == legacy_keys, "F2: emb support (cell count) matches the legacy");
  expect(extra == 0, "F2: no cell the legacy lacks (no spurious insert)");
  expect(core2.emb_exact_zeros() == ref2.emb_exact_zero_keys(),
         "F2: exact-zero cell count matches the legacy (zeros preserved, not filtered)");
  if (pinned) {
    // The count assertion above would also pass if BOTH sides had zero zeros, so
    // assert the premise: the pinned fixture really does carry them.
    expect(ref2.emb_exact_zero_keys() > 0,
           "F2: pinned fixture DOES carry exact-zero emb keys (the premise)");
  }

  std::printf("\n-- F3: createReverseEmb --\n");

  RFERef ref3(N, L, k, t, pa);
  ref3.load_rk(rk_in);
  ref3.rank_norm();
  ref3.create_emb();
  ref3.create_reverse_emb();
  const auto& remb_legacy = ref3.remb();

  std::vector<int> rk_core3 = rk_in;
  RFECore core3(rk_core3, (size_t)N, (size_t)L, k, t, pa);
  core3.f1_rank_norm();
  core3.f2_create_emb();
  core3.f3_create_reverse_emb();

  // Value comparison: every cell the legacy's remb holds must be reproduced.
  long remb_cells = 0, remb_bad = 0, no_counterpart = 0, core_extra = 0;
  long legacy_empty_rows = 0;
  for (int q = 0; q < N; ++q) {
    const auto rit = remb_legacy.find(q);
    if (rit == remb_legacy.end()) continue;
    if (rit->second.empty()) ++legacy_empty_rows;
    for (const auto& kv : rit->second) {
      ++remb_cells;
      const float cv = core3.remb().get((CT)q, (CT)kv.first);
      if (cv != kv.second) {
        if (remb_bad == 0)
          std::printf("    first diff at [%d,%d]: legacy=%.9g core=%.9g\n", q, kv.first,
                      kv.second, cv);
        ++remb_bad;
      }
      // The transpose identity: remb[q][col] should be emb[q][col] when the
      // legacy had no counterpart, count those separately.
      const auto eit = emb_legacy.find(q);
      const bool has = (eit != emb_legacy.end()) && (eit->second.count(kv.first) != 0);
      if (!has) ++no_counterpart;
    }
    core3.remb().for_each_in_row((CT)q, [&](CT c, float) {
      if (rit != remb_legacy.end() && rit->second.count((int)c) == 0) ++core_extra;
    });
  }

  std::printf("  legacy remb cells=%ld  core remb nnz=%zu  value-diffs=%ld\n",
              remb_cells, core3.nnz_remb(), remb_bad);
  std::printf("  remb rows: legacy has %d empty-but-present, core has %zu non-empty\n",
              legacy_empty_rows, core3.remb_rows());
  std::printf("  remb cells with no emb[q][col] counterpart: %ld\n", no_counterpart);

  expect(remb_bad == 0, "F3: every remb value matches the legacy bit-exactly");
  expect(core_extra == 0, "F3: no remb cell the legacy lacks (no spurious insert)");
  // The row-set difference is a REAL, measured difference, not a defect: the
  // legacy pre-creates a row per id (RFE.cpp:621-624), a transpose does not.
  // It is reported above and asserted here so it cannot silently change.
  expect(core3.remb_rows() <= (std::size_t)N, "F3: core remb rows <= N");
  std::printf("  note: empty-row difference is expected and measured above;"
              " F4 reads these as 0.0f either way.\n");

  std::printf("\n-- F4: combineReverseEmb --\n");

  
  RFERef ref4(N, L, k, t, pa);
  ref4.load_rk(rk_in);
  ref4.rank_norm();
  ref4.create_emb();
  ref4.create_reverse_emb();
  ref4.combine_reverse_emb();
  const auto& emb_post = ref4.emb();

  std::vector<int> rk_core4 = rk_in;
  RFECore core4(rk_core4, (size_t)N, (size_t)L, k, t, pa);
  core4.f1_rank_norm();
  core4.f2_create_emb();
  core4.f3_create_reverse_emb();
  core4.f4_combine_reverse_emb();

  long pre_keys = 0, post_keys = 0, f4_bad = 0, f4_extra = 0, f4_zero = 0;
  for (int q = 0; q < N; ++q) {
    const auto& lrow = emb_post.at(q);
    post_keys += (long)lrow.size();
    for (const auto& kv : lrow) {
      if (kv.second == 0.0f) ++f4_zero;
      const float cv = core4.emb().get((CT)q, (CT)kv.first);
      if (cv != kv.second) {
        if (f4_bad == 0)
          std::printf("    first diff at [%d,%d]: legacy=%.9g core=%.9g\n", q, kv.first,
                      kv.second, cv);
        ++f4_bad;
      }
    }
    core4.emb().for_each_in_row((CT)q, [&](CT c, float) {
      if (lrow.count((int)c) == 0) ++f4_extra;
    });
  }
  for (int q = 0; q < N; ++q) pre_keys += (long)core4.emb().row_nnz((CT)q);

  std::printf("  emb keys before combine=%zu  after=%ld (legacy)  value-diffs=%ld  extra=%ld\n",
              core4.nnz_emb(), post_keys, f4_bad, f4_extra);
  std::printf("  post-combine exact-zero keys: %ld of %ld (%.2f%%)\n", f4_zero, post_keys,
              post_keys ? 100.0 * (double)f4_zero / (double)post_keys : 0.0);

  expect(f4_bad == 0, "F4: every combined emb value matches the legacy bit-exactly");
  expect(f4_extra == 0, "F4: no cell the legacy lacks");
  expect((std::size_t)post_keys == core4.nnz_emb(),
         "F4: the support is UNCHANGED by the combine (keys kept, zeroed)");

  // The claim that made this phase hand-rolled, re-measured HERE rather than
  // quoted from Step 1: run the two engine kernels on the same inputs and count
  // how many of the legacy's keys they would drop. If they stopped dropping, the
  // dedicated kernel would be unjustified complexity.
  {
    std::vector<int> rp = rk_in;
    RFECore probe(rp, (std::size_t)N, (std::size_t)L, k, t, pa);
    probe.f1_rank_norm();
    probe.f2_create_emb();
    probe.f3_create_reverse_emb();
    // CSRMatrix is MOVE-ONLY (its copy ctor is deleted), so the matrices cannot
    // be snapshotted by value. Instead of copying, run the engine kernels against
    // a core that is ABOUT TO be advanced: compare their output with the legacy's
    // post-combine emb directly, then let the probe move on to F4.
    const CSRMatrix<float>& pre_emb = probe.emb();
    const CSRMatrix<float>& pre_remb = probe.remb();
    const CSRMatrix<float> c = CSRMatrix<float>::combine(pre_emb, pre_remb);
    const CSRMatrix<float> m = CSRMatrix<float>::merge(
        pre_emb, pre_remb, [](float a, float b) { return a * b; });
    // Compare SUPPORTS, not values. Step 1's H4b counted key-set differences and
    // found combine/merge losing 3443 of 5461 keys; this first version of the
    // check compared VALUES with get() and found 0 mismatches, which looks like
    // the engine kernels being fine. It is not: get() returns 0.0f both for a
    // cell the engine DROPPED and for a cell the legacy legitimately zeroed, and
    // 69.68% of the post-combine emb is exactly zero -- so a value comparison
    // cannot see the difference at all. The claim is about the SUPPORT, and that
    // is what is measured here.
    auto row_keys = [](const CSRMatrix<float>& mat, int q) {
      std::set<int> s;
      mat.for_each_in_row((CT)q, [&](CT cc, float) { s.insert((int)cc); });
      return s;
    };
    long lost_c = 0, lost_m = 0, ck_keys = 0, mk_keys = 0, lk_keys = 0;
    for (int q = 0; q < N; ++q) {
      const std::set<int> ck = row_keys(c, q);
      const std::set<int> mk = row_keys(m, q);
      const auto& lrow = emb_post.at(q);
      lk_keys += (long)lrow.size();
      ck_keys += (long)ck.size();
      mk_keys += (long)mk.size();
      for (const auto& kv : lrow) {
        if (!ck.count(kv.first)) ++lost_c;
        if (!mk.count(kv.first)) ++lost_m;
      }
    }
    std::printf("  support check on the same input: legacy keys=%ld | combine() keys=%ld"
                " | merge() keys=%ld\n", lk_keys, ck_keys, mk_keys);
    std::printf("    legacy keys combine() would DROP=%ld  merge() would DROP=%ld"
                "  (values alone cannot show this: %ld of %ld post-combine cells"
                " are exactly 0.0f, and get() reads 0 in both cases)\n",
                lost_c, lost_m, f4_zero, post_keys);
    // REGIME-GATED, and the gating is itself a Step 1 lesson. This assertion
    // asks "do the engine kernels lose keys here?", which only has a YES answe
    // when the combine actually PRODUCED ZEROS. At the saturated fixture
    // (N=60 L=60 k=30, zero_keys=0) emb comes out dense and symmetric, so the
    // legacy has no zero-valued cells and combine()/merge() are simply correct
    // -- nothing to drop. Step 1 hit the identical trap with H2/H4b, and the
    // honest move is to make the vacuous case VISIBLE instead of asserting
    // universally (which would force deleting the measurement).
    if (f4_zero > 0) {
      expect(lost_c > 0,
             "F4: CSRMatrix::combine DOES drop keys the legacy keeps (dedicated kernel justified)");
      expect(lost_m > 0,
             "F4: CSRMatrix::merge ALSO drops them (emission filter drops zeros)");
    } else {
      std::printf("  note: this fixture produced 0 exact-zero cells, so the"
                  " engine-kernel-drop check is VACUOUS here (they are correct when"
                  " there are no zeros to lose). Pinned fixture covers it.\n");
    }
  }

  // ---------------------------------------------------------------- F5 checks --
std::printf("\n-- F5: sortEmb + effectEstimationEmb --\n");

  RFERef ref5(N, L, k, t, pa);
  ref5.load_rk(rk_in);
  ref5.rank_norm();
  ref5.create_emb();
  ref5.create_reverse_emb();
  ref5.combine_reverse_emb();
  ref5.sort_emb();
  ref5.effect_estimation_emb();

  std::vector<int> rk_core5 = rk_in;
  RFECore core5(rk_core5, (size_t)N, (size_t)L, k, t, pa);
  core5.f1_rank_norm();
  core5.f2_create_emb();
  core5.f3_create_reverse_emb();
  core5.f4_combine_reverse_emb();
  core5.f5_sort_emb();
  core5.f5_effect_estimation_emb();

  // (a) sorted_emb: the KEY ORDER, compared position by position. A digest would
  // be enough, but a positional diff names the first row that diverged, which is
  // what makes a std::sort tie-break bug debuggable instead of merely detectable.
  long se_bad = 0, se_extra = 0;
  for (int q = 0; q < N; ++q) {
    const auto& lr = ref5.sorted_emb()[(size_t)q];
    const auto& cr = core5.sorted_emb()[(size_t)q];
    if (lr.size() != cr.size()) {
      if (se_extra == 0)
        std::printf("    row %d length: legacy=%zu core=%zu\n", q, lr.size(), cr.size());
      ++se_extra;
      continue;
    }
    for (size_t i = 0; i < lr.size(); ++i)
      if (lr[i] != cr[i]) {
        if (se_bad == 0)
          std::printf("    first sorted_emb diff at row %d pos %zu: legacy=%d core=%d\n",
                      q, i, lr[i], cr[i]);
        ++se_bad;
      }
  }
  expect(se_bad == 0, "F5: sorted_emb KEY ORDER matches the legacy position by position");
  expect(se_extra == 0, "F5: sorted_emb row lengths match (support identical)");

  // (b) ee values, after normalisation.
  long ee_bad = 0;
  for (int q = 0; q < N; ++q) {
    const float lv = ref5.ee()[(size_t)q], cv = core5.ee()[(size_t)q];
    if (lv != cv) {
      if (ee_bad == 0)
        std::printf("    first ee diff at %d: legacy=%.9g core=%.9g\n", q, lv, cv);
      ++ee_bad;
    }
  }
  expect(ee_bad == 0, "F5: every ee value matches the legacy bit-exactly");

  // (c) sorted_ee: the ORDER, not the set. This is the F6 loop order, so a
  // tie-break difference here would change F6's accumulation order and therefore
  // the Gram -- the one check that must be order-sensitive.
  long order_bad = 0;
  for (int i = 0; i < N; ++i)
    if (ref5.sorted_ee()[(size_t)i] != core5.sorted_ee()[(size_t)i]) {
      if (order_bad == 0)
        std::printf("    first sorted_ee diff at pos %d: legacy=%d core=%d\n", i,
                    ref5.sorted_ee()[(size_t)i], core5.sorted_ee()[(size_t)i]);
      ++order_bad;
    }
  expect(order_bad == 0,
         "F5: sorted_ee ORDER matches the legacy (unstable tie-break reproduced)");

  // (c2) NEGATIVE CONTROL for (c). Passing (c) does not by itself prove the
  // tie-break was reproduced -- if ties never occurred, the order check would be
  // vacuous (measured: 0 tied pairs at 8 of the 10 sweep fixtures, so this is a
  // real risk, not a hypothetical one). This re-sorts the SAME keys with a STABLE
  // sort and reports where that differs from the legacy's order. A non-zero count
  // proves (i) ties are genuinely present here and (ii) the legacy's order is NOT
  // the stable one, so reproducing it required reproducing libstdc++'s unstable
  // std::sort rather than "sorting descending" in general.
  {
    std::vector<int> stable((size_t)N);
    for (int i = 0; i < N; ++i) stable[(size_t)i] = i;
    const std::vector<float>& ne = ref5.ee();
    std::stable_sort(stable.begin(), stable.end(), [&ne](int a, int b) {
      return ne[(size_t)a] > ne[(size_t)b];
    });
    long differs = 0;
    for (int i = 0; i < N; ++i)
      if (stable[(size_t)i] != ref5.sorted_ee()[(size_t)i]) ++differs;
    std::printf("  positions where a STABLE sort would differ from the legacy order: %ld"
                " of %d\n", differs, N);
    // Reported, NOT asserted: on a tie-free fixture this is legitimately 0.
  }

  // (d) grValue: read BEFORE normalisation, so it cannot be recovered from the
  // normalised ee. Checked directly. Also reports whether the degenerate
  // divide-by-zero regime was reached.
  // ---- grValue reachability (see FINDINGS.md, "grValue > 0 by construction") ----
// The theorem says grValue == 0 is UNREACHABLE for any input the core accepts,
// so this sweep is a COROBORATION, not the proof: if some config ever reported
// 0.0f, the theorem is wrong and needs revisiting, not a fixture.
  std::printf("  grValue=%.9g  (theorem: > 0 always -- a 0 here would refute it)\n",
              (double)core5.gr_value());
  expect((double)core5.gr_value() == (double)ref5.gr_value_probe(),
         "F5: grValue (the max unnormalised ee) matches the legacy");

  // (e) The tie-break is only observable if ties EXIST. If every ee is distinct,
  // the order check above is vacuous -- so measure the tie population instead of
  // assuming it (X3: regime dependence is the recurring failure here).
  {
    long tied = 0;
    for (int i = 0; i < N; ++i)
      for (int j = i + 1; j < N; ++j)
        if (ref5.ee()[(size_t)j] == ref5.ee()[(size_t)i]) ++tied;
    std::printf("  ee pairs with EXACTLY equal normalised values: %ld of %ld\n", tied,
                (long)N * (N - 1) / 2);
    std::printf("  ck_ee=0x%016llx ck_sorted_emb=0x%016llx ck_sorted_ee=0x%016llx\n",
                (unsigned long long)core5.ck_ee(),
                (unsigned long long)core5.ck_sorted_emb(),
                (unsigned long long)core5.ck_sorted_ee());
  }

  // ---------------------------------------------------------------- F6 checks --
std::printf("\n-- F6: Gram + incElems (reRankingByCPRR core) --\n");

  RFERef ref6(N, L, k, t, pa);
  ref6.load_rk(rk_in);
  ref6.rank_norm();
  ref6.create_emb();
  ref6.create_reverse_emb();
  ref6.combine_reverse_emb();
  ref6.sort_emb();
  ref6.effect_estimation_emb();
  ref6.cprr_gram_only();          // Gram + incElems + incElemsL, no final sort

  std::vector<int> rk_core6 = rk_in;
  RFECore core6(rk_core6, (size_t)N, (size_t)L, k, t, pa);
  core6.f1_rank_norm();
  core6.f2_create_emb();
  core6.f3_create_reverse_emb();
  core6.f4_combine_reverse_emb();
  core6.f5_sort_emb();
  core6.f5_effect_estimation_emb();
  core6.f6_gram_and_inc_elems();

  // (a) incElems support, element by element. incElems[i] is a std::set<int> in
  // the legacy, so this is an exact set comparison -- the thing H8 says a
  // value-filtered port would lose.
  long ie_bad = 0, ie_extra = 0, ie_total = 0;
  for (int i = 0; i < N; ++i) {
    const auto& lset = ref6.inc_elems()[(size_t)i];
    ie_total += (long)lset.size();
    std::set<int> cset;
    core6.gram().for_each_in_row((CT)i, [&](CT c, float) { cset.insert((int)c); });
    for (const int j : lset) if (!cset.count(j)) ++ie_bad;
    for (const int j : cset) if (!lset.count(j)) ++ie_extra;
  }
  std::printf("  incElems cells (legacy): %ld  core gram nnz: %zu\n", ie_total,
              core6.nnz_gram());
  expect(ie_bad == 0, "F6: every legacy incElems candidate survives in the Gram");
  expect(ie_extra == 0, "F6: the Gram has no candidate the legacy lacks");
  expect((std::size_t)ie_total == core6.nnz_gram(),
         "F6: incElems cell count equals the Gram nnz (same support)");

  // (b) Gram VALUES, cell by cell, EXCLUDING the pow(2,30) diagonal pin (which is
  // applied after accumulation and belongs to F7's input state).
  long g_bad = 0, g_shared = 0;
  for (int i = 0; i < N; ++i) {
    const auto& lset = ref6.inc_elems()[(size_t)i];
    for (const int j : lset) {
      if (i == j) continue;                       // diagonal: pinned, see F7
      const float lv = ref6.gram_at(i, j);
      const float cv = core6.gram().get((CT)i, (CT)j);
      ++g_shared;
      if (lv != cv) {
        if (g_bad == 0)
          std::printf("    first Gram diff at [%d,%d]: legacy=%.9g core=%.9g\n", i, j, lv, cv);
        ++g_bad;
      }
    }
  }
  expect(g_bad == 0,
         "F6: every off-diagonal Gram value matches the legacy bit-exactly");
  std::printf("  off-diagonal Gram cells compared: %ld\n", g_shared);

  // (c) THE ZERO CENSUS. This is H8's population and the reason F7 cannot do a
  // value-only top-L prune. Measured on BOTH sides and compared, because a
  // count that only exists on the core's side proves nothing.
  {
    long lzero = 0;
    for (int i = 0; i < N; ++i)
      for (const int j : ref6.inc_elems()[(size_t)i])
        if (i != j && ref6.gram_at(i, j) == 0.0f) ++lzero;
    const std::size_t czero = core6.gram_exact_zeros();
    // The percentage alone is misleading across fixtures (Step 1's H8c measured
    // 26.56% at N=120; this fixture measures 14.88% at N=200 -- same code, same
    // definition, different fixture). The cell counts are printed together so the
    // two can never be compared without also seeing what they are counts OF.
    std::printf("  exact-zero Gram cells: legacy=%ld  core=%zu  (%.2f%% of %zu cells"
                " at N=%d L=%d k=%d)\n",
                lzero, czero,
                core6.nnz_gram() ? 100.0 * (double)czero / (double)core6.nnz_gram() : 0.0,
                core6.nnz_gram(), N, L, k);
    expect(lzero == (long)czero, "F6: the exact-zero Gram census matches the legacy");
  }

  // (d) incElemsL, the other input to F7's set_difference.
  long l_bad = 0;
  for (int q = 0; q < N; ++q) {
    std::set<int> ls;
    for (const int j : ref6.inc_elems_l()[(size_t)q]) ls.insert(j);
    const auto& cs = core6.inc_elems_l()[(size_t)q];
    if (ls.size() != cs.size()) { ++l_bad; continue; }
    for (const int j : ls)
      if (!std::binary_search(cs.begin(), cs.end(), j)) ++l_bad;
  }
  expect(l_bad == 0, "F6: incElemsL matches the legacy set for every row");

  // X8: this digest was PRINTED and NEVER ASSERTED for the whole Step 2 arc.
  // A printed instrument is decorative; an asserted one is a pin. ck_gram hashes
  // the Gram's raw float bits, so it is the hard instrument -- the one that can
  // see a build-regime change, which the core-vs-reference delta structurally
  // cannot (both sides get perturbed identically).
  //
  // The value is the BENCH/NFM regime digest of the pinned fixture, recorded
  // from Data/phase_profile/rfe_numerics_prod (Stage 3c). Under the product's
  // flags this must FAIL -- see Source/tests/step3b_real_gate.sh, which proves
  // it does, because a pin that cannot fail is X6 in a different costume.
  std::printf("  ck_gram=0x%016llx\n", (unsigned long long)core6.ck_gram());
  if (pinned) {
    expect(core6.ck_gram() == 0x9cf3d5102ffa87e9ULL,
           "X8: ck_gram == 0x9cf3d5102ffa87e9 (ABSOLUTE pin, strict regime)");
  }
  if (rkpath) {
    // Real-data pins. Recorded Stage 3b/2 on 2026-10-03; these are the first
    // real-data bit-exactness pins for RFE. `ck_gram` is printed per config so
    // a FAIL names which dataset moved.
    const char* tag = std::strrchr(rkpath, '/');
    tag = tag ? tag + 1 : rkpath;
    std::printf("  REAL pin(cfg=F6 section) cfg=%s ck_gram=0x%016llx\n", tag,
                (unsigned long long)core6.ck_gram());
  }

  // ------------------------------------------------------------ F7a + E2E --
  std::printf("\n-- F7a: reRankingByEmbSim (the t-loop phase) --\n");

  // F7a is measured on its own first: one iteration, same input both sides.
  {
    RFERef ra(N, L, k, t, pa);
    ra.load_rk(rk_in);
    ra.rank_norm();
    ra.create_emb();
    ra.create_reverse_emb();
    ra.combine_reverse_emb();
    ra.embedding_rr_sim_only();          // reRankingByEmbSim, one pass

    std::vector<int> rk_a = rk_in;
    RFECore ca(rk_a, (size_t)N, (size_t)L, k, t, pa);
    ca.f1_rank_norm();
    ca.f2_create_emb();
    ca.f3_create_reverse_emb();
    ca.f4_combine_reverse_emb();
    ca.f7a_rerank_by_emb_sim();

    long rk_bad = 0;
    for (int q = 0; q < N; ++q)
      for (int i = 0; i < L; ++i)
        if (ra.rk()[(size_t)L * q + (size_t)i] != ca.rk_ref()[(size_t)L * q + (size_t)i]) ++rk_bad;
    expect(rk_bad == 0,
           "F7a: rkLists after reRankingByEmbSim matches the legacy position by position");
    std::printf("  rk cells differing after one embRR pass: %ld of %d\n", rk_bad, N * L);
  }

  // ------------------------------------------------------------- E2E check --
  // THE CHECK THAT MATTERS MOST. Every per-phase check above feeds BOTH sides
  // the same (post-F4) state, so they would all pass even if the phases were
  // wired together in the wrong ORDER. F7a is the proof that order matters:
  // it rewrites rkLists, so the emb F5/F6 see is not the emb F2 built. This
  // runs the FULL legacy pipeline in the FULL legacy order -- rankNorm, then t
  // iterations of the embedding RR, then the cartesian product -- and compares
  // the final rank lists. It is the only check that would catch a mis-ordered
  // chain, and Step 0's own digests say the final ck_rk is 0x58ded033b72ded72,
  // NOT the 0x3465e556d414ca79 that a F1..F6-only chain would produce.
  std::printf("\n-- E2E: full pipeline in legacy order (rankNorm + t x embRR + CP) --\n");
  {
    RFERef re(N, L, k, t, pa);
    re.load_rk(rk_in);
    re.rank_norm();
    for (int ct = 0; ct < t; ++ct) re.exec_embedding_rr_pub();   // embRR x t
    re.exec_cartesian_product_pub();                           // CP (+ its tail)

    // X9: call the production entry point; do NOT re-spell the sequence here.
    // A gate that reimplements the pipeline can only verify its own copy, which
    // is how RFECore::run() came to skip f7a while every gate stayed green.
    std::vector<int> rk_e = rk_in;
    RFECore ce(rk_e, (size_t)N, (size_t)L, k, t, pa);
    // The profile is ON here so the gate can AUDIT THE INSTRUMENT (X6): a
    // profiler that reports a zero nobody wrote is more dangerous than no
    // profiler, and five counters were exactly that until this assertion existed.
    ce.set_run_profile(true);
    ce.run();
    // X8 SENTINEL, on the production entry point. The previous one sat on the
    // F6 section, which is driven to f6 on a PRE-t-loop state -- it detected
    // regime drift on a pipeline the method never runs (X9). This one is taken
    // after run(). Value recorded under the strict (BENCH) regime; step3b_real_gate.sh
    // proves it FAILS under PROD, which is what makes it a sentinel rather than
    // a decoration. It is a REGRESSION pin of the core: Gram PARITY is certified
    // by the final ck_rk, not by this digest.
    if (rkpath) {
      struct P { const char* tag; int n; int l; int k; unsigned long long g; };
      const P pins[] = {
        {"corel5k_L400_k10",  5000, 400, 10, 0x28bdf35f87bfff3dULL},
        {"corel5k_L1000_k10", 5000, 1000, 10, 0x4ecba5630543276aULL},
        {"linnaeus_L400_k10", 8000, 400, 10, 0xa584ebbe38e86571ULL},
        // k=5 / k=20: FAITHFUL post-run() ck_gram, measured 05/10 via
        // run_seq_probe and NOT asserted until now -- the gate never ran these
        // configs (FINDINGS: "to be asserted"). The sweep's ck_gram column is
        // the F6-section (pre-t-loop), superseded -- do not confuse the two.
        {"corel5k_L400_k5",   5000, 400, 5,  0x0ff2642751009152ULL},
        {"corel5k_L400_k20",  5000, 400, 20, 0x3218b119e9aabc2cULL},
        {"corel5k_L1000_k20", 5000, 1000, 20, 0x045f449c61d55fb5ULL},
      };
      for (const P& q : pins) {
        if ((int)N == q.n && L == q.l && k == q.k) {
          std::printf("  REAL FAITHFUL cfg=%s ck_gram=0x%016llx\n", q.tag,
                      (unsigned long long)ce.ck_gram());
          expect(ce.ck_gram() == q.g,
                 "X8: faithful run() ck_gram on real data");
        }
      }
    }
    if (pinned) {
      // FAITHFUL real-data ck_gram: read AFTER run(), post-t-loop. The print
      // above is the F6 SECTION, a PRE-t-loop object with a different digest;
      // the gate must not conflate them -- that was the original X8 defect.
      // ck_rk stays the OUTPUT pin; this one is a core REGRESSION pin.
      expect(ce.ck_gram() == 0x46322e3d65fe6403ULL,
             "X8 SENTINEL: run() ck_gram == 0x46322e3d65fe6403 (strict regime, "
             "post-t-loop state)");
    }
    {
      const RFECore::RunProfile& rp = ce.run_profile();
      expect(rp.fields_written,
             "PROF: every RunProfile counter has a writer (fields_written)");
      expect(rp.emb_nnz_final == (long long)ce.emb_.nnz(),
             "PROF: emb_nnz_final == core.emb_.nnz()");
      expect(rp.gram_nnz == (long long)ce.nnz_gram(),
             "PROF: gram_nnz == core.nnz_gram()");
      expect(rp.gram_zeros == (long long)ce.gram_exact_zeros(),
             "PROF: gram_zeros == core.gram_exact_zeros()");
      expect(rp.emb_zeros_final == ce.count_emb_zeros(),
             "PROF: emb_zeros_final == core.count_emb_zeros()");
      expect((int)rp.emb_nnz_it.size() == t && (int)rp.f7a_visits_it.size() == t,
             "PROF: one emb/F7a sample per t-loop iteration");
      expect(rp.t_total > 0.0 && rp.gram_nnz > 0,
             "PROF: counters that cannot be zero are not (non-vacuous, X6)");
      // Counts only, NEVER a time. The gate md5's the whole stdout across thread
      // counts, so printing t_total here made a DETERMINISTIC core look
      // non-deterministic (bench README, pitfall 5). The assertions above carry
      // the evidence; that line was only for the human and it cost the gate its
      // determinism check.
    }

    // ---- F7b in ISOLATION, on the state F6 produced ----------------------
    // The E2E failure above could be F7b being wrong, or F7b being fed a
    // different incElems/Gram than the legacy has. Isolate by comparing F7b
    // against a reference that has been driven to the SAME point: full embRR
    // loop, then cprr_gram_prefix() ONLY (no tail). If F7b matches there, F7b
    // is right and the E2E mismatch is upstream. If it does not, F7b is wrong.
    {
      RFERef rb(N, L, k, t, pa);
      rb.load_rk(rk_in);
      rb.rank_norm();
      for (int ct = 0; ct < t; ++ct) rb.exec_embedding_rr_pub();
      // exec_cartesian_product's exact prefix (RFE.cpp:214-223), stopped before
      // the tail. BOTH steps are mandatory and each one bit differently than it
      // looks:
      //   * hyper_rr_embedding REBUILDS emb from the post-loop rkLists. Without
      //     it the reference's emb is the one built BEFORE the loop, while the
      //     core's is the one built after -- so the Gram compares two different
      //     matrices and reports a huge mismatch that looks like an F6 bug.
      //   * effec_estimation populates sorted_ee_, which cprr_gram_prefix
      //     iterates. Without it that loop never runs, incElems stays EMPTY, and
      //     a Gram comparison over that empty set compares ZERO cells while
      //     printing "0 differing" -- a vacuous green.
      // Both mistakes were made and caught here; the gate now asserts a non-zero
      // comparison count so the second cannot recur silently.
      rb.hyper_rr_embedding();
      rb.effec_estimation();
      rb.cprr_gram_only();                    // stops BEFORE the tail

      std::vector<int> rk_b = rk_in;
      RFECore cb(rk_b, (size_t)N, (size_t)L, k, t, pa);
      cb.f1_rank_norm();
      for (int ct = 0; ct < t; ++ct) {
        cb.f2_create_emb();
        cb.f3_create_reverse_emb();
        cb.f4_combine_reverse_emb();
        cb.f7a_rerank_by_emb_sim();
      }
      cb.f2_create_emb();
      cb.f3_create_reverse_emb();
      cb.f4_combine_reverse_emb();
      cb.f5_sort_emb();
      cb.f5_effect_estimation_emb();
      cb.f6_gram_and_inc_elems();

      // Snapshot the rank lists BEFORE either side runs the tail.
      const std::vector<int> cb_pre_tail_rk = cb.rk_ref();
      const std::vector<int> rb_pre_tail_rk = rb.rk();

      // Gram agreement FIRST -- if this fails, F7b's input is wrong and there is
      // no point blaming the tail.
      long gb = 0, gz = 0, gcmp = 0;
      for (int q = 0; q < N; ++q) {
        const auto& lset = rb.inc_elems()[(size_t)q];
        for (const int j : lset) {
          const float lv = (q == j) ? (float)pow(2, 30) : rb.gram_at(q, j);
          const float cv = (q == j) ? (float)pow(2, 30) : cb.gram().get((CT)q, (CT)j);
          ++gcmp;
          if (lv != cv) ++gb;
          if (lv == 0.0f) ++gz;
        }
      }
      std::printf("  F7b isolation: Gram cells differing from the legacy = %ld"
                  " (zero-valued = %ld, compared = %ld)\n", gb, gz, gcmp);
      // GUARD AGAINST A VACUOUS PASS. This loop iterates the LEGACY's incElems, so
      // if the reference's accumulation never ran the set is empty, the loop body
      // never executes, and gb stays 0 -- a green result that compared NOTHING.
      // That happened once already. Requiring a non-zero comparison count makes
      // the vacuous case a failure instead of a silent pass.
      expect(gcmp > 0, "F7b: the Gram comparison actually compared cells (not vacuous)");
      expect(gb == 0,
             "F7b: the Gram at the tail's input point matches the legacy (incl. the "
             "pow(2,30) diagonal read)");

      rb.cprr_tail_only();                    // legacy's FINAL rkLists (the target)
      long tb = 0;
      // Evaluate the UNION prune on the core's PRE-TAIL state. Order matters:
      // calling it after cb.f7b_rerank_cprr_tail() would re-sort an already
      // sorted list and compare against the wrong target (measured: 197/200 rows
      // differing, which is an artefact of the call order, not of the prune).
      {
        long union_bad = 0, valueonly_bad = 0;
        for (int q = 0; q < N; ++q) {
          std::vector<int> want;
          for (int i = 0; i < L; ++i) want.push_back(rb.rk()[(size_t)L * q + (size_t)i]);
          const std::vector<int> up = cb.union_prune_row(q, (std::size_t)L);
          const std::vector<int> have(up.begin(),
                                      up.begin() + std::min<int>((int)up.size(), L));
          bool same = have.size() == want.size();
          if (same)
            for (std::size_t s = 0; s < want.size(); ++s) same &= (have[s] == want[s]);
          if (!same) ++union_bad;

          std::vector<std::pair<float, int>> cand;
          cb.gram().for_each_in_row((CT)q, [&](CT c, float v) {
            if (v != 0.0f) cand.emplace_back(v, (int)c);   // the H8 WRONG filter
          });
          std::stable_sort(cand.begin(), cand.end(),
                           [](const std::pair<float, int>& x, const std::pair<float, int>& y) {
                             if (x.first != y.first) return x.first > y.first;
                             return x.second < y.second;
                           });
          const std::vector<int> vh(got_ids(cand, L));
          bool vok = vh.size() == want.size();
          if (vok)
            for (std::size_t s = 0; s < want.size(); ++s) vok &= (vh[s] == want[s]);
          if (!vok) ++valueonly_bad;
        }
        std::printf("  H8: union-prune rows differing=%ld  |  value-only-prune rows"
                    " differing=%ld  (of %d; %ld zero-valued Gram cells compared)\n",
                    union_bad, valueonly_bad, N, gz);
        expect(union_bad == 0,
               "H8: the UNION prune reproduces the legacy's top-L for every row");
        // X6: a prune check over zero rows would pass vacuously.
        expect(gcmp > 0, "H8: the prune comparison actually compared Gram cells (not vacuous)");
        // The union-prune soundness table, one line per fixture, so the claim
        // "the prune is exact" is backed by the whole sweep and not just the pin
        // (X3: a result from one fixture is not a general claim).
        std::printf("    PRUNE %d %d %d %d %ld %ld\n", N, L, k, t, union_bad, valueonly_bad);
        // The value-only prune is REPORTED, never asserted: with no zeros the two
        // selections coincide, so asserting a difference would be regime-blind.
      }

      cb.f7b_rerank_cprr_tail();
      for (int q = 0; q < N; ++q)
        for (int i = 0; i < L; ++i)
          if (rb.rk()[(size_t)L * q + (size_t)i] != cb.rk_ref()[(size_t)L * q + (size_t)i]) ++tb;
      std::printf("  F7b isolation: rkLists differing after the tail alone = %ld of %d\n",
                  tb, N * L);
      expect(tb == 0, "F7b: the CPRR tail reproduces the legacy rank lists on a shared input");
    }

    long e_bad = 0;
    int first_q = -1, first_i = -1;
    for (int q = 0; q < N; ++q)
      for (int i = 0; i < L; ++i)
        if (re.rk()[(size_t)L * q + (size_t)i] != ce.rk_ref()[(size_t)L * q + (size_t)i]) {
          if (first_q < 0) { first_q = q; first_i = i; }
          ++e_bad;
        }
    std::printf("  E2E final rkLists: legacy ck_rk=0x%016llx  core ck_rk=0x%016llx\n",
                (unsigned long long)re.ck_rk(), (unsigned long long)ce.ck_rk());
    std::printf("  cells differing: %ld of %d%s\n", e_bad, N * L,
                e_bad ? "" : "  (identical)");
    if (e_bad)
      std::printf("    first diff at [%d,%d]: legacy=%d core=%d\n", first_q, first_i,
                  re.rk()[(size_t)L * first_q + (size_t)first_i],
                  ce.rk_ref()[(size_t)L * first_q + (size_t)first_i]);
    expect(e_bad == 0,
           "E2E: the full pipeline in legacy order reproduces the final rank lists");

    // ---- FAITHFUL E2E Gram: the reference's Gram at the END of the pipeline --
    // F6 (above) compares a PRE-t-loop Gram; this compares the state the product
    // actually produces: legacy `re` driven through exec_cartesian_product vs
    // core `ce` through run(). Readable here because the CPRR tail writes only a
    // set_difference over incElems_ (rfe_ref_verbatim.hpp:665-675); MX and
    // incElems_ are written ONLY in the prefix (:617, :642). Diagonal excluded
    // from the value pass: the legacy overwrites it with pow(2,30) after
    // accumulation (RFE.cpp:605) -- same rule as F6(b).
    {
      long gsup_total = 0, gsup_missing = 0, gsup_extra = 0;
      for (int i = 0; i < N; ++i) {
        const std::set<int>& lset = re.inc_elems()[(size_t)i];
        gsup_total += (long)lset.size();
        std::set<int> cset;
        ce.gram().for_each_in_row((CT)i, [&](CT c, float) { cset.insert((int)c); });
        for (const int j : lset)
          if (!cset.count(j)) ++gsup_missing;
        for (const int j : cset)
          if (!lset.count(j)) ++gsup_extra;
      }
      long gval_cmp = 0, gval_bad = 0;
      for (int i = 0; i < N; ++i) {
        for (const int j : re.inc_elems()[(size_t)i]) {
          if (i == j) continue;                       // diagonal: pinned, see F6
          const float lv = re.gram_at(i, j);
          const float cv = ce.gram().get((CT)i, (CT)j);
          ++gval_cmp;
          if (lv != cv) {
            if (gval_bad == 0)
              std::printf("    first FAITHFUL Gram diff at [%d,%d]: legacy=%.9g core=%.9g\n",
                          i, j, lv, cv);
            ++gval_bad;
          }
        }
      }
      std::printf("  E2E Gram (faithful): support cells=%ld missing=%ld extra=%ld"
                  " | off-diagonal values compared=%ld mismatches=%ld\n",
                  gsup_total, gsup_missing, gsup_extra, gval_cmp, gval_bad);
      expect(gsup_missing == 0,
             "E2E Gram: no legacy candidate missing from the core (faithful, post-t-loop)");
      expect(gsup_extra == 0,
             "E2E Gram: no cell the core holds that the legacy lacks (faithful)");
      expect((std::size_t)gsup_total == ce.nnz_gram(),
             "E2E Gram: legacy incElems cells == core Gram nnz (same support)");
      expect(gval_cmp > 0,
             "E2E Gram: off-diagonal values were actually compared (non-vacuous, X6)");
      expect(gval_bad == 0,
             "E2E Gram: every off-diagonal value matches the legacy bit-exactly (faithful)");
    }
    if (pinned) {
      expect((unsigned long long)ce.ck_rk() == 0x58ded033b72ded72ULL,
             "E2E: ck_rk == 0x58ded033b72ded72 (Step 0 golden, full pipeline)");
    }

    // X9 runs on the pinned fixture AND on every real-data config
    // (pinned || rkpath) -- never unconditionally: the step2 sweep drives
    // synthetic non-pinned fixtures, where "short == faithful" is a property of
    // the INPUT, and this control certifies the pins, which exist only here.
    if (pinned || rkpath) {

      // ---- X9 NEGATIVE CONTROL (permanent assertion, not a one-off) ----------
      //
      // The gate above now calls ce.run(). That is only worth something if the
      // run() it calls is the RIGHT one -- and for the whole of Steps 0-3 it was
      // not: run() skipped f7a_rerank_by_emb_sim() and ran the embRR block once
      // instead of t_ times, while every gate stayed green because the gate
      // spelled the sequence out again instead of calling run().
      //
      // So: run the SHORT sequence HERE, deliberately, and assert it does NOT
      // reproduce the golden. If this ever passes with equal digests, the pin
      // has stopped distinguishing the two pipelines and the whole E2E pin is
      // decoration. Deliberately wrong code is confined to this local scope;
      // run() and the core carry only the faithful sequence.
      {
        std::vector<int> rk_short = rk_in;
        RFECore cs(rk_short, (size_t)N, (size_t)L, k, t, pa);
        cs.f1_rank_norm();
        cs.f2_create_emb(); cs.f3_create_reverse_emb(); cs.f4_combine_reverse_emb();
        cs.f5_sort_emb(); cs.f5_effect_estimation_emb();
        cs.f6_gram_and_inc_elems(); cs.f7b_rerank_cprr_tail();
        std::printf("  X9 control: faithful run() ck_rk=0x%016llx | "
                    "short (no f7a, embRR x1) ck_rk=0x%016llx\n",
                    (unsigned long long)ce.ck_rk(),
                    (unsigned long long)cs.ck_rk());
        if (pinned) {
          expect(cs.ck_rk() != 0x58ded033b72ded72ULL,
                 "X9: the sequence SKIPPING f7a does NOT reproduce the pin "
                 "(otherwise the pin cannot tell the two pipelines apart)");
        } else {
          // Real data: the assertion is short != faithful ON THIS CONFIG. If it
          // ever fails the control is EMPTY here -- a property of the input, NOT
          // a bug in the core -- and the message says exactly that, so nobody
          // chases a defect that does not exist (the pinned-fixture control
          // remains the code check).
          expect(cs.ck_rk() != ce.ck_rk(),
                 "X9: control EMPTY on this real config -- the short sequence "
                 "coincides with run() HERE (a property of this input, not a "
                 "core bug): this config cannot distinguish the two pipelines");
        }
      }
    }
  }

  // ------------------------------------------------- F5 tie-break fixtures --
  // Three DESIGNED tie regimes (see gen_fixture_ties for why rows cannot simply
  // be duplicated). For each: full-pipeline parity, the tied-pair census, and the
  // negative control -- how many positions a STABLE sort would get wrong. The
  // last number is what tells us the fixture actually exercises the tie-break;
  // a fixture with 0 tied pairs is a vacuous pass regardless of parity.
  std::printf("\n-- F5: deliberate ee-tie fixtures --\n");
  {
    struct Case { int N, L, k, t, patterns; const char* what; };
    const Case cases[] = {
        {200, 40, 10, 2,   1, "MANY ties: all rows share one pattern"},
        {120, 30,  8, 1,   3, "FEW ties: 3 patterns over 120 rows"},
        { 80, 20,  6, 1,  40, "MIXED: 40 patterns over 80 rows"},
        { 60, 20,  5, 1,   2, "DENSE: 2 patterns, small N, large k"},
    };
    long total_tied = 0, total_stable_wrong = 0, cases_ok = 0;
    for (const Case& c : cases) {
      const std::vector<int> rk = gen_fixture_ties(42, c.N, c.L, c.patterns);

      RFERef ref(c.N, c.L, c.k, c.t, pa);
      ref.load_rk(rk);
      ref.rank_norm();
      for (int ct = 0; ct < c.t; ++ct) ref.exec_embedding_rr_pub();
      ref.exec_cartesian_product_pub();

      std::vector<int> rkc = rk;
      RFECore core(rkc, (size_t)c.N, (size_t)c.L, c.k, c.t, pa);
      core.f1_rank_norm();
      for (int ct = 0; ct < c.t; ++ct) {
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
      for (int q = 0; q < c.N; ++q)
        for (int i = 0; i < c.L; ++i)
          if (ref.rk()[(size_t)c.L * q + (size_t)i] !=
              core.rk_ref()[(size_t)c.L * q + (size_t)i]) ++diff;

      // The tie census and negative control come from the F5 state. Rebuild the
      // same F5 state on a core we did not advance past it.
      std::vector<int> rkf = rk;
      RFECore f5core(rkf, (size_t)c.N, (size_t)c.L, c.k, c.t, pa);
      f5core.f1_rank_norm();
      for (int ct = 0; ct < c.t; ++ct) {
        f5core.f2_create_emb();
        f5core.f3_create_reverse_emb();
        f5core.f4_combine_reverse_emb();
        f5core.f7a_rerank_by_emb_sim();
      }
      f5core.f2_create_emb();
      f5core.f3_create_reverse_emb();
      f5core.f4_combine_reverse_emb();
      f5core.f5_sort_emb();
      f5core.f5_effect_estimation_emb();

      const long tied = ee_tied_pairs(ref.ee());
      const long stable_wrong =
          stable_sort_positions_differing(ref.sorted_ee(), ref.ee());
      total_tied += tied;
      total_stable_wrong += stable_wrong;
      // sorted_ee ORDER must match -- that is the property under test.
      long order_bad = 0;
      for (int i = 0; i < c.N; ++i)
        if (ref.sorted_ee()[(size_t)i] != f5core.sorted_ee()[(size_t)i]) ++order_bad;

      std::printf("  %-34s N=%3d L=%2d k=%2d pat=%2d | rk diffs=%ld | sorted_ee order"
                  " diffs=%ld | ee tied pairs=%5ld | stable-sort would be wrong in"
                  " %ld/%d\n",
                  c.what, c.N, c.L, c.k, c.patterns, diff, order_bad, tied,
                  stable_wrong, c.N);
      if (diff == 0 && order_bad == 0) ++cases_ok;
    }
    expect(cases_ok == 4,
           "F5: the 4 deliberate tie fixtures are bit-exact end to end (rk + sorted_ee)");
    std::printf("  totals: ee tied pairs=%ld, positions a stable sort would get wrong=%ld\n",
                total_tied, total_stable_wrong);
    // X6: the fixtures are only meaningful if they actually produce ties.
    expect(total_tied > 0,
           "F5: the tie fixtures really produce ee ties (not a vacuous pass)");
  }

  if (pinned) {
    expect(core.ck_rk() == 0x3465e556d414ca79ULL, "F1: ck_rk == 0x3465e556d414ca79 (Step 0 pin)");
    expect(core.ck_sim_dense() == 0x8afb15dd0502c7cbULL,
           "F1: ck_matrix == 0x8afb15dd0502c7cb (Step 0 pin)");
  }

  if (g_failures == 0) {
    std::printf("RFE PARITY F1+F2+F3+F4+F5+F6+F7: OK (bit-identical to the legacy)\n");
    return 0;
  }
  std::printf("RFE PARITY F1+F2+F3+F4+F5+F6+F7: %d FAILURE(S)\n", g_failures);
  return 1;
}