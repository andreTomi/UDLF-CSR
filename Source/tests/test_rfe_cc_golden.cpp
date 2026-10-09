// test_rfe_cc_golden.cpp
//
// CC golden for the PERFORM_CCS port. Runs the VERBATIM legacy CC pipeline
// (RFERef::exec_rr_ccs phases, Source/tests/rfe_ref_verbatim.hpp) on the
// structured fixtures of Data/fixtures/cc/ and RECORDS per-stage digests.
// These digests are the pins the port (RFECore F8*) must reproduce; this file
// is the ONLY producer of them -- never regenerate a pin from the port itself.
//
// Every case runs TWICE in a fresh RFERef and all digests must be identical
// (intra-process reproducibility). Two directed tests run as well:
//   H11 -- the processCCs bound is ((int)E)*th_cc in DOUBLE, never truncated
//          to int before the comparison; an independent replay (own DSU, own
//          bound arithmetic) must reproduce icc/rcc exactly, for a fractional
//          bound and for the automatic th_cc=0 path.
//   H14 -- unionCCs' set_union-inserter-into-its-own-source behaves as a true
//          set union with the q-side label surviving; checked against an
//          independent DSU on a directed sequence (chain, star, merges).
//
// embycc sign/finitude (H16) is asserted, not assumed: any negative or
// non-finite value FAILS the golden, because the sparse embycc variants of
// Etapa 6 are only valid under that premise.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp -I Source/tests
//       Source/tests/test_rfe_cc_golden.cpp -o /tmp/test_rfe_cc_golden
// Run from the repo root: /tmp/test_rfe_cc_golden

#include "rfe_ref_verbatim.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void expect(bool c, const char* what) {
  if (!c) {
    std::printf("  FAIL: %s\n", what);
    ++g_fail;
  }
}

// The repo's FNV-1a instrument, non-canonical offset basis on purpose (same
// literal the Step 0 golden and RFECore use, so digests are comparable).
constexpr std::uint64_t BASIS = 1469598103934665603ULL;
std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t nb) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < nb; ++i) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return h;
}
std::uint64_t h_u64(std::uint64_t h, std::uint64_t v) { return fnv(h, &v, sizeof v); }
std::uint64_t h_f(std::uint64_t h, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);  // hash the BITS: -0.0/NaN must not collapse
  return h_u64(h, bits);
}

std::vector<int> load_bin(const std::string& path, std::size_t expect_n) {
  std::vector<int> rk(expect_n, 0);
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::printf("FAIL: cannot open %s\n", path.c_str());
    ++g_fail;
    return rk;
  }
  f.read(reinterpret_cast<char*>(rk.data()), (std::streamsize)(expect_n * sizeof(int)));
  if ((std::size_t)f.gcount() != expect_n * sizeof(int)) {
    std::printf("FAIL: short read %s\n", path.c_str());
    ++g_fail;
  }
  return rk;
}

struct Case {
  const char* name;
  int n, l, k, t;
  double pa, th;  // th = PARAM_RFE_TH_CC input (0.0 = legacy automatic path)
};

struct Digests {
  std::uint64_t rk_pre = 0, edges = 0, icc = 0, ccemb = 0, srtcc = 0,
                embycc = 0, matrix = 0, inc = 0, rk_fin = 0;
  double th_out = 0.0;
  bool operator==(const Digests& o) const {
    return rk_pre == o.rk_pre && edges == o.edges && icc == o.icc &&
           ccemb == o.ccemb && srtcc == o.srtcc && embycc == o.embycc &&
           matrix == o.matrix && inc == o.inc && rk_fin == o.rk_fin &&
           th_out == o.th_out;
  }
};

// Runs the full legacy path on one fixture x th_cc case and returns every
// stage digest. print=true emits the stats block the Etapa-0/6 tables quote.
Digests run_case(const Case& c, bool print) {
  Digests d;
  const std::string path = std::string("Data/fixtures/cc/") + c.name + ".bin";
  std::vector<int> rk = load_bin(path, (std::size_t)c.n * (std::size_t)c.l);
  if (g_fail) return d;

  RFERef ref(c.n, c.l, c.k, c.t, c.pa);  // fresh per case: th_cc_ etc. start clean
  ref.load_rk(rk);
  ref.set_th_cc(c.th);
  ref.run();  // rankNorm -> t x embRR -> cartesian product (F1..F7b)
  d.rk_pre = ref.ck_rk();

  // -- createEdgesCCS: the FULL sorted edge vector (prefix selection is the
  //    next stage's job; the vector itself is a stage output).
  ref.create_edges_ccs();
  const auto& E = ref.edges_ccs();
  d.edges = BASIS;
  for (const auto& e : E) {
    d.edges = h_f(d.edges, e.first);
    d.edges = h_u64(d.edges, (std::uint32_t)e.second.first);
    d.edges = h_u64(d.edges, (std::uint32_t)e.second.second);
  }

  // -- processCCs: labels + threshold + component census.
  ref.process_ccs();
  d.th_out = ref.th_cc();
  d.icc = BASIS;
  for (int q = 0; q < c.n; ++q) d.icc = h_u64(d.icc, (std::uint32_t)ref.icc().at(q));
  long long ncc = (long long)ref.rcc().size(), ncc_ge2 = 0;
  std::vector<int> sizes;
  for (const auto& [r, s] : ref.rcc()) {
    if (s.size() > 1) ++ncc_ge2;
    sizes.push_back((int)s.size());
  }
  std::sort(sizes.rbegin(), sizes.rend());

  // -- computeCCsEmb (+ sortCCEmb inside).
  ref.compute_ccs_emb();
  d.ccemb = BASIS;
  long long ccemb_nnz = 0, ccemb_zeros = 0, ccemb_ties = 0;
  for (const auto& [r, m] : ref.ccemb()) {
    d.ccemb = h_u64(d.ccemb, (std::uint32_t)r);
    float prev = 0.0f;
    bool first = true;
    for (const auto& [key, v] : m) {
      d.ccemb = h_u64(d.ccemb, (std::uint32_t)key);
      d.ccemb = h_f(d.ccemb, v);
      ++ccemb_nnz;
      if (v == 0.0f) ++ccemb_zeros;
      if (!first && v == prev) ++ccemb_ties;
      prev = v;
      first = false;
    }
  }
  d.srtcc = BASIS;
  for (int r = 0; r < c.n; ++r) {
    const auto& v = ref.sorted_ccemb()[(std::size_t)r];
    d.srtcc = h_u64(d.srtcc, v.size());
    for (int key : v) d.srtcc = h_u64(d.srtcc, (std::uint32_t)key);
  }
  for (const auto& [r, m] : ref.ccemb())
    expect(ref.sorted_ccemb()[(std::size_t)r].size() == m.size(),
           "sorted_ccemb[r].size() == ccemb[r].size()");

  // -- computeEmbsByCCs: dense over CCs; H16 asserts sign and finitude.
  ref.compute_embs_by_ccs();
  d.embycc = BASIS;
  long long ebz = 0, eneg = 0, enonf = 0, eb_nnz = 0;
  for (int q = 0; q < c.n; ++q) {
    auto it = ref.embycc().find(q);
    expect(it != ref.embycc().end(), "embycc carries every object q");
    if (it == ref.embycc().end()) continue;
    d.embycc = h_u64(d.embycc, it->second.size());
    for (const auto& [r, v] : it->second) {
      d.embycc = h_u64(d.embycc, (std::uint32_t)r);
      d.embycc = h_f(d.embycc, v);
      ++eb_nnz;
      if (v == 0.0f) ++ebz;
      if (v < 0.0f) ++eneg;
      if (!std::isfinite(v)) ++enonf;
    }
  }
  expect(eneg == 0, "H16: embycc has no negative value (sparse-embycc premise)");
  expect(enonf == 0, "H16: embycc is entirely finite");

  // -- Replay of reRankingByCCs' CELL loop (pre-tail): write counts per cell,
  //    cross-CC collisions, getPosition-absent rate, rows touched.
  std::map<std::uint64_t, int> writes;
  long long total_w = 0, absent = 0, coll_cells = 0;
  std::set<int> rows;
  for (const auto& it : ref.ccemb()) {
    const int r = it.first;
    const int m = std::min(c.k, (int)it.second.size());
    for (int i = 0; i < m; ++i) {
      const int k1 = ref.sorted_ccemb()[(std::size_t)r][(std::size_t)i];
      rows.insert(k1);
      for (int j = 0; j < m; ++j) {
        const int k2 = ref.sorted_ccemb()[(std::size_t)r][(std::size_t)j];
        ++writes[(std::uint64_t)k1 * (std::uint64_t)c.n + (std::uint64_t)k2];
        ++total_w;
        if (ref.get_position(k1, k2) == (unsigned)(c.l + 2)) ++absent;
      }
    }
  }
  for (const auto& [kk, v] : writes)
    if (v > 1) ++coll_cells;

  // -- reRankingByCCs: cells + tail.
  ref.reranking_by_ccs();
  d.matrix = fnv(BASIS, ref.matrix().data(), ref.matrix().size() * sizeof(float));
  d.inc = BASIS;
  long long inc_tot = 0;
  for (const auto& s : ref.inc_elems()) {
    d.inc = h_u64(d.inc, s.size());
    for (int x : s) d.inc = h_u64(d.inc, (std::uint32_t)x);
    inc_tot += (long long)s.size();
  }
  d.rk_fin = ref.ck_rk();

  bool self0 = true;
  for (int q = 0; q < c.n; ++q)
    if (ref.rk()[(std::size_t)q * c.l] != q) self0 = false;
  expect(self0, "self stays at position 0 after the CC tail");

  if (print) {
    std::printf("CASE %s n=%d l=%d k=%d t=%d th_in=%.9g\n", c.name, c.n, c.l, c.k,
                c.t, c.th);
    std::printf("  rk_pre=0x%016llx  edges E=%zu digest=0x%016llx\n",
                (unsigned long long)d.rk_pre, E.size(),
                (unsigned long long)d.edges);
    std::printf("  th_out=%.9g icc=0x%016llx ncc=%lld ncc_ge2=%lld sizes>=",
                d.th_out, (unsigned long long)d.icc, ncc, ncc_ge2);
    for (std::size_t i = 0; i < sizes.size() && i < 8; ++i)
      std::printf("%d,", sizes[i]);
    std::printf("\n");
    std::printf("  ccemb nnz=%lld zeros=%lld adjacent_ties=%lld digest=0x%016llx"
                " sorted_ccemb=0x%016llx\n",
                ccemb_nnz, ccemb_zeros, ccemb_ties, (unsigned long long)d.ccemb,
                (unsigned long long)d.srtcc);
    std::printf("  embycc nnz=%lld zeros=%lld neg=%lld nonf=%lld digest=0x%016llx\n",
                eb_nnz, ebz, eneg, enonf, (unsigned long long)d.embycc);
    std::printf("  cells writes=%lld coll_cells=%lld absent_lplus2=%lld rows=%zu\n",
                total_w, coll_cells, absent, rows.size());
    std::printf("  matrix=0x%016llx incElems nnz=%lld inc=0x%016llx"
                " rk_fin=0x%016llx\n",
                (unsigned long long)d.matrix, inc_tot, (unsigned long long)d.inc,
                (unsigned long long)d.rk_fin);
  }
  return d;
}

// Independent DSU replicating unionCCs' SEMANTICS without its set_union
// trick: membership is a true set union; the label of o1's component
// survives. Comparing the reference against this on the same edge sequence
// is exactly H14.
struct RefDSU {
  std::map<int, std::set<int>> comp;
  std::map<int, int> label;
  void init(int n) {
    comp.clear();
    label.clear();
    for (int q = 0; q < n; ++q) {
      comp[q] = std::set<int>{q};
      label[q] = q;
    }
  }
  void unite(int o1, int o2) {
    const int r1 = label[o1], r2 = label[o2];
    comp[r1].insert(comp[r2].begin(), comp[r2].end());
    for (int o : comp[r2]) label[o] = r1;
    comp.erase(r2);
  }
};

bool same_partition(const RFERef& ref, const RefDSU& dsu) {
  for (const auto& [q, l] : dsu.label)
    if (ref.icc().at(q) != l) return false;
  if (ref.rcc().size() != dsu.comp.size()) return false;
  for (const auto& [r, s] : dsu.comp) {
    auto it = ref.rcc().find(r);
    if (it == ref.rcc().end() || it->second != s) return false;
  }
  return true;
}

// H14: chain, star, cross-merge -- unionCCs must match the DSU at every step.
void h14() {
  RFERef ref(8, 4, 2, 1, 0.1);
  ref.set_th_cc(1.0);  // skip the automatic average path (no ee_ yet)
  ref.init_edges_ccs();
  RefDSU dsu;
  dsu.init(8);
  const std::pair<int, int> seq[] = {{0, 1}, {1, 2}, {2, 3}, {5, 4}, {5, 3},
                                     {5, 2}, {3, 4}, {7, 6}, {4, 6}, {6, 0}};
  for (const auto& e : seq) {
    ref.union_ccs(e.first, e.second);
    dsu.unite(e.first, e.second);
    if (!same_partition(ref, dsu)) {
      expect(false, "H14: partition diverged mid-sequence");
      return;
    }
  }
  expect(same_partition(ref, dsu), "H14: unionCCs == true union, q-side label survives");
  expect(ref.same_cc(0, 7) && !ref.same_cc(0, 1) == false,
         "H14: sameCC agrees with the DSU");
}

// H11: the bound is ((int)E)*th_cc in DOUBLE. Synthetic cyclic rows give
// E = n*(k-1) exactly; th=1/3 makes the truncated variant lose one edge.
void h11_fractional() {
  const int n = 26, l = 5, k = 5;  // E = 26*4 = 104
  std::vector<int> rk((std::size_t)n * (std::size_t)l, 0);
  for (int q = 0; q < n; ++q)
    for (int i = 0; i < l; ++i) rk[(std::size_t)q * l + i] = (q + i) % n;
  RFERef ref(n, l, k, 2, 0.1);
  ref.load_rk(rk);
  ref.set_th_cc(1.0 / 3.0);
  ref.run();
  ref.create_edges_ccs();
  ref.process_ccs();
  const auto& E = ref.edges_ccs();
  const double th = ref.th_cc();
  const double bound = (double)((int)E.size() * th);
  const double bound_trunc = (double)(int)((double)E.size() * th);
  RefDSU dsu;
  dsu.init(n);
  long long processed = 0;
  for (const auto& t : E) {
    if (processed >= ((int)E.size() * th)) break;
    const int a = t.second.first, b = t.second.second;
    if (dsu.label[a] != dsu.label[b]) dsu.unite(a, b);
    ++processed;
  }
  std::printf("H11 fractional: E=%zu th=%.17g bound=%.17g bound_trunc=%.17g"
              " processed=%lld\n",
              E.size(), th, bound, bound_trunc, processed);
  expect(same_partition(ref, dsu),
         "H11: reference prefix+unions == independent replay (double bound)");
}

// H11 automatic path: th_cc=0 -> the average rule; replay with the OBSERVED
// th_out, so the auto-computed value itself is inside the checked surface.
void h11_auto() {
  const int n = 26, l = 5, k = 5;
  std::vector<int> rk((std::size_t)n * (std::size_t)l, 0);
  for (int q = 0; q < n; ++q)
    for (int i = 0; i < l; ++i) rk[(std::size_t)q * l + i] = (q + i) % n;
  RFERef ref(n, l, k, 2, 0.1);
  ref.load_rk(rk);
  ref.set_th_cc(0.0);
  ref.run();
  ref.create_edges_ccs();
  ref.process_ccs();
  const double th = ref.th_cc();
  expect(th > 0.0, "H11 auto: th_cc=0 path produced a positive threshold");
  const auto& E = ref.edges_ccs();
  RefDSU dsu;
  dsu.init(n);
  long long processed = 0;
  for (const auto& t : E) {
    if (processed >= ((int)E.size() * th)) break;
    const int a = t.second.first, b = t.second.second;
    if (dsu.label[a] != dsu.label[b]) dsu.unite(a, b);
    ++processed;
  }
  std::printf("H11 auto: th_out=%.17g E=%zu processed=%lld\n", th, E.size(),
              processed);
  expect(same_partition(ref, dsu),
         "H11 auto: reference prefix+unions == independent replay");
}

}  // namespace

int main() {
  const Case cases[] = {
      {"cc_degenerate", 200, 40, 10, 2, 0.1, 0.0},   // one giant CC
      {"cc_structured", 500, 100, 10, 2, 0.1, 0.0},  // automatic th_cc
      {"cc_structured", 500, 100, 10, 2, 0.1, 2.0},  // th>1: every edge processed
      {"cc_th_cc_0", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_th_cc_05", 500, 100, 10, 2, 0.1, 0.05},
      {"cc_th_cc_50", 500, 100, 10, 2, 0.1, 0.5},
      {"cc_th_cc_100", 500, 100, 10, 2, 0.1, 1.0},
      {"cc_k2", 500, 100, 2, 2, 0.1, 0.0},
      {"cc_k20", 500, 100, 20, 2, 0.1, 0.0},
      {"cc_k50", 500, 100, 50, 2, 0.1, 0.0},
      {"cc_k100", 500, 100, 100, 2, 0.1, 0.0},  // k == l
      // H19 self-displaced: pins for the CC stages on off0 input (Etapa 8/9
      // will compare the port's final rk against THESE).
      {"cc_selfoff_early", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_selfoff_late", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_selfoff_mixed", 500, 100, 10, 2, 0.1, 0.5},
  };
  for (const auto& c : cases) {
    const Digests a = run_case(c, true);
    const Digests b = run_case(c, false);
    expect(a == b, "REPRO: two fresh runs give identical digests");
  }
  h14();
  h11_fractional();
  h11_auto();
  if (g_fail) {
    std::printf("RFE CC GOLDEN: %d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("RFE CC GOLDEN: OK\n");
  return 0;
}