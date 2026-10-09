// test_rfe_cc_parity.cpp -- parity gate for the PERFORM_CCS port.
//
// GROWS ONE STAGE PER ETAPA (never two changes measured together). Current
// coverage:
//   Etapa 2 (F8a) -- after f7b, re-running f2,f3,f4,f5 in the core must
//   reproduce the legacy's post-CPRR embedding re-derivation bit-exactly:
//   execRRCCs starts with doHyperRREmbedding + doHyperEffecEstimation over the
//   ALREADY REWRITTEN rank lists (RFE.cpp:226-227), so `emb`, `sorted_emb`,
//   `ee` and `sorted_ee` used by every CC phase are NOT the pre-CPRR ones.
//   Etapa 3 (F8b) -- edges (both variants A/B), th_cc (auto + fixed), and the
//   prefix count, each against the legacy's own vector and loop semantics.
//   Etapa 4 (F8d) -- components: icc/rcc for both variants (literal, DSU).
//   Etapa 5 (F8e) -- ccemb/sorted_ccemb: content FULLY identical for
//   accum-A and accum-B with full sort; sorted prefix-m for the fast sort.
// Later etapas append: embycc (6, DONE: F8f B-csr + dense A), reranking cells (7), rk final (8).
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp -I Source/tests
//       Source/tests/test_rfe_cc_parity.cpp -o /tmp/test_rfe_cc_parity
// Run from the repo root.

#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using CT = CSRMatrix<float>::size_type;

constexpr std::uint64_t BASIS = 1469598103934665603ULL;
std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t nb) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < nb; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

int g_fail = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++g_fail;
  } else {
    std::printf("  ok  : %s\n", what);
  }
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
  const char* name;  // fixture in Data/fixtures/cc ("" = synthetic cyclic)
  int n, l, k, t;
  double pa;
  double th;  // PARAM_RFE_TH_CC input (0.0 = legacy automatic rule)
};

// Etapa 2's whole check on one fixture: both engines re-derive the embedding
// over the post-f7b rank lists; every observable must be bit-identical.
void run_case(const Case& c) {
  std::printf("CASE %s n=%d l=%d k=%d t=%d\n", c.name ? c.name : "cyclic", c.n,
              c.l, c.k, c.t);
  std::vector<int> rk_ref((std::size_t)c.n * (std::size_t)c.l, 0);
  if (c.name) {
    rk_ref = load_bin(std::string("Data/fixtures/cc/") + c.name + ".bin",
                      rk_ref.size());
  } else {
    for (int q = 0; q < c.n; ++q)
      for (int i = 0; i < c.l; ++i)
        rk_ref[(std::size_t)q * c.l + i] = (q + i) % c.n;  // self at 0, distinct
  }
  if (g_fail) return;
  std::vector<int> rk_core = rk_ref;

  // Legacy side: the exact prefix of execRRCCs (RFE.cpp:226-227).
  RFERef ref(c.n, c.l, c.k, c.t, c.pa);
  ref.set_th_cc(c.th);
  ref.load_rk(rk_ref);
  ref.run();
  ref.hyper_rr_embedding();
  ref.effec_estimation();  // sortEmb + effectEstimationEmb (RFE.cpp:220-223)

  // Core side: run() ends at f7b; the CC entry will re-run the same phases.
  RFECore core(rk_core, (std::size_t)c.n, (std::size_t)c.l, c.k, c.t, c.pa);
  core.set_th_cc(c.th);
  core.run();
  core.f2_create_emb();
  core.f3_create_reverse_emb();
  core.f4_combine_reverse_emb();
  core.f5_sort_emb();
  core.f5_effect_estimation_emb();

  // (a) sorted_emb: key order position by position. Length equality plus
  //     element equality pins the SUPPORT (exact-zero keys included) and the
  //     unstable tie order at once.
  long se_bad = 0, se_len = 0;
  for (int q = 0; q < c.n; ++q) {
    const auto& lr = ref.sorted_emb()[(std::size_t)q];
    const auto& cr = core.sorted_emb()[(std::size_t)q];
    if (lr.size() != cr.size()) {
      if (se_len == 0)
        std::printf("    row %d length: legacy=%zu core=%zu\n", q, lr.size(),
                    cr.size());
      ++se_len;
      continue;
    }
    for (std::size_t i = 0; i < lr.size(); ++i)
      if (lr[i] != cr[i]) {
        if (se_bad == 0)
          std::printf("    first sorted_emb diff at row %d pos %zu\n", q, i);
        ++se_bad;
      }
  }
  expect(se_len == 0, "F8a: sorted_emb row lengths match (support identical)");
  expect(se_bad == 0, "F8a: sorted_emb KEY ORDER matches position by position");

  // (b) emb values: every legacy key, bit-exact, through the CSR get().
  long emb_bad = 0;
  for (int q = 0; q < c.n; ++q) {
    auto it = ref.emb().find(q);
    if (it == ref.emb().end()) continue;
    for (const auto& [key, v] : it->second) {
      const float cv = core.emb().get((CT)q, (CT)key);
      if (cv != v) {
        if (emb_bad == 0)
          std::printf("    first emb diff at [%d,%d]: legacy=%.9g core=%.9g\n",
                      q, key, v, cv);
        ++emb_bad;
      }
    }
  }
  expect(emb_bad == 0, "F8a: every emb value matches the legacy bit-exactly");

  // (c) ee, bit-exact (order-sensitive: same accumulation, same inputs).
  long ee_bad = 0;
  for (int q = 0; q < c.n; ++q)
    if (ref.ee()[(std::size_t)q] != core.ee()[(std::size_t)q]) {
      if (ee_bad == 0)
        std::printf("    first ee diff at %d: legacy=%.9g core=%.9g\n", q,
                    (double)ref.ee()[(std::size_t)q],
                    (double)core.ee()[(std::size_t)q]);
      ++ee_bad;
    }
  expect(ee_bad == 0, "F8a: every ee value matches the legacy bit-exactly");

  // (d) sorted_ee order -- initEdgesCCs' automatic th_cc averages IN THIS
  //     ORDER (RFE.cpp:350-352), so it is load-bearing for the CC port.
  long see_bad = 0;
  for (int q = 0; q < c.n; ++q)
    if (ref.sorted_ee()[(std::size_t)q] != core.sorted_ee()[(std::size_t)q]) {
      if (see_bad == 0) std::printf("    first sorted_ee diff at pos %d\n", q);
      ++see_bad;
    }
  expect(see_bad == 0, "F8a: sorted_ee ORDER matches the legacy");

  // ---- Etapa 3 (F8b): edges + th_cc + prefix ------------------------------
  ref.create_edges_ccs();
  ref.process_ccs();  // triggers the automatic th_cc when th_in == 0
  const double th_ref = ref.th_cc();
  const auto& re = ref.edges_ccs();

  auto check_edges = [&](const auto& core_edges, const char* tag) {
    long e_len = 0, e_bad = 0;
    if (re.size() != core_edges.size()) {
      std::printf("    [%s] edges length: legacy=%zu core=%zu\n", tag,
                  re.size(), core_edges.size());
      ++e_len;
    } else {
      for (std::size_t i = 0; i < re.size(); ++i) {
        std::uint32_t lb, cb;
        std::memcpy(&lb, &re[i].first, 4);
        std::memcpy(&cb, &core_edges[i].first, 4);
        if (lb != cb || re[i].second != core_edges[i].second) {
          if (e_bad == 0)
            std::printf("    [%s] first edge diff at %zu: legacy=(%.9g,%d,%d) "
                        "core=(%.9g,%d,%d)\n",
                        tag, i, (double)re[i].first, re[i].second.first,
                        re[i].second.second, (double)core_edges[i].first,
                        core_edges[i].second.first, core_edges[i].second.second);
          ++e_bad;
        }
      }
    }
    std::string m1 = std::string("F8b[") + tag + "]: edge count matches the legacy";
    std::string m2 = std::string("F8b[") + tag +
                     "]: edge vector identical (conf BITS + pairs, order)";
    expect(e_len == 0, m1.c_str());
    expect(e_bad == 0, m2.c_str());
  };

  core.f8a_create_edges_ccs();  // variant A (serial, default)
  const double th_core = core.f8b_init_th_cc();
  expect(th_ref == th_core, "F8b: th_cc bit-identical (auto rule and fixed input)");
  check_edges(core.edges_ccs(), "A");

  // Variant B (parallel chunks): identical by construction (same emission
  // sequence); verified against the legacy anyway, never assumed.
  core.set_f8_edges_parallel(true);
  core.f8a_create_edges_ccs();
  core.set_f8_edges_parallel(false);
  check_edges(core.edges_ccs(), "B");

  // Prefix: the core's closed form vs the legacy's OWN loop semantics replayed
  // over the legacy's own vector (H11 pins the loop; this pins the closed form).
  long long m_ref = 0;
  for (const auto& t : re) {
    (void)t;
    if (m_ref >= ((int)re.size() * th_ref)) break;
    ++m_ref;
  }
  const long long m_core = (long long)core.f8c_edge_prefix();
  if (m_ref != m_core)
    std::printf("    prefix: legacy-loop=%lld core-closed-form=%lld\n", m_ref,
                m_core);
  expect(m_ref == m_core, "F8b: prefix length == legacy loop count");

  // ---- H30 (Etapa 6b item 3): closed form EXACT, no epsilon -----------------
  // The legacy loop processes edges while i < (int)E * th, i.e. ceil(E*th). An
  // epsilon is WRONG: a bound just above an integer (34.00000000000001) must
  // consume 35 edges, but ceil(prod - 1e-12) returns 34. Replay the loop as
  // ground truth and compare; also show a th where the old epsilon diverges.
  {
    auto legacy_loop = [](long long E, double th) {
      long long i = 0;
      for (long long tt = 0; tt < E; ++tt) {
        if (i >= (double)((int)E) * th) break;
        ++i;
      }
      return i;
    };
    bool sweep_ok = true;
    const long long E = 100;
    for (int j = 0; j <= 2000; ++j) {
      const double th = (double)j / 2000.0 * 0.6;  // 0 .. 0.6, crosses integers
      const long long want = legacy_loop(E, th);
      const long long got = RFECore::f8_prefix_closed_form(E, th);
      if (want != got) {
        if (sweep_ok)
          std::printf("    H30 sweep: j=%d th=%.17g loop=%lld closed=%lld\n", j,
                      th, want, got);
        sweep_ok = false;
      }
    }
    expect(sweep_ok, "F8b[H30]: closed form == legacy loop across a th sweep");
    // Directed: th one ulp above 0.34 -> E*th just above 34 -> 35 edges.
    const double th_crit = std::nextafter(0.34, 1.0);
    const long long directed_loop = legacy_loop(E, th_crit);
    const long long directed = RFECore::f8_prefix_closed_form(E, th_crit);
    if (directed != 35)
      std::printf("    H30 directed: th=%.17g loop=%lld closed=%lld (want 35)\n",
                  th_crit, directed_loop, directed);
    expect(directed == directed_loop && directed == 35,
           "F8b[H30]: bound just above 34 consumes 35 edges (not 34)");
  }

  // ---- Etapa 4 (F8d): components -------------------------------------------
  // ref.process_ccs() already ran in the Etapa-3 block above; its icc/rcc are
  // the oracle. Both core variants must reproduce them exactly -- labels AND
  // the induced grouping (rcc is derivable from icc, which is checked too, so
  // no map/set structure in the core needs to be exposed).
  auto check_labels = [&](const std::vector<int>& lab, const char* tag) {
    long bad = 0;
    for (int q = 0; q < c.n; ++q)
      if (ref.icc().at(q) != lab[(std::size_t)q]) {
        if (bad == 0)
          std::printf("    [%s] first icc diff at %d: legacy=%d core=%d\n", tag,
                      q, ref.icc().at(q), lab[(std::size_t)q]);
        ++bad;
      }
    std::map<int, std::set<int>> grp;
    for (int q = 0; q < c.n; ++q) grp[lab[(std::size_t)q]].insert(q);
    long rcc_bad = 0;
    if (grp.size() != ref.rcc().size()) {
      rcc_bad = 1;
      std::printf("    [%s] rcc size: legacy=%zu core-grouping=%zu\n", tag,
                  ref.rcc().size(), grp.size());
    } else {
      for (const auto& [r, s] : grp) {
        auto it = ref.rcc().find(r);
        if (it == ref.rcc().end() || it->second != s) { ++rcc_bad; break; }
      }
    }

    std::string m1 = std::string("F8d[") + tag + "]: icc labels identical (per object)";
    std::string m2 = std::string("F8d[") + tag + "]: rcc grouping identical (sets per label)";
    expect(bad == 0, m1.c_str());
    expect(rcc_bad == 0, m2.c_str());
  };

  core.set_f8_components_dsu(false);  // force variant A (default may be B)
  core.f8d_process_ccs();             // variant A (literal map/set)
  check_labels(core.f8_cc_label(), "A");
  core.set_f8_components_dsu(true);
  core.f8d_process_ccs();  // variant B (DSU)
  core.set_f8_components_dsu(false);
  check_labels(core.f8_cc_label(), "B");

  // ---- Etapa 5 (F8e): ccemb + sorted_ccemb ---------------------------------
  // The reference computes them inside compute_ccsEmb (which calls sortCCEmb).
  ref.compute_ccs_emb();

  auto check_ccemb = [&](const char* tag, bool prefix_only, long long fallbacks) {
    long bad = 0, sbad = 0, extra = 0;
    for (int r = 0; r < c.n; ++r) {
      auto it = ref.ccemb().find(r);
      const auto& row = core.ccemb_rows()[(std::size_t)r];
      const auto& srt = core.sorted_ccemb()[(std::size_t)r];
      if (it == ref.ccemb().end()) {
        if (!row.empty() || !srt.empty()) {
          if (extra == 0)
            std::printf("    [%s] unexpected non-empty core row at label %d\n",
                        tag, r);
          ++extra;
        }
        continue;
      }
      const auto& rmap = it->second;  // std::map: ascending key
      if (row.size() != rmap.size()) {
        if (bad == 0)
          std::printf("    [%s] ccemb size at %d: legacy=%zu core=%zu\n", tag,
                      r, rmap.size(), row.size());
        ++bad;
        continue;
      }
      {
        std::size_t idx = 0;
        bool rowbad = false;
        for (const auto& kv : rmap) {
          std::uint32_t lb, cb;
          std::memcpy(&lb, &kv.second, 4);
          std::memcpy(&cb, &row[idx].second, 4);
          if (row[idx].first != kv.first || lb != cb) {
            if (!rowbad)
              std::printf(
                  "    [%s] first ccemb diff at label %d key %d: "
                  "legacy=%.9g core=%.9g (core key %d)\n",
                  tag, r, kv.first, (double)kv.second,
                  (double)row[idx].second, row[idx].first);
            rowbad = true;
            break;
          }
          ++idx;
        }
        if (rowbad) ++bad;
      }
      const auto& rs = ref.sorted_ccemb()[(std::size_t)r];
      bool sbad_row = false;
      if (prefix_only) {
        const std::size_t m = std::min((std::size_t)c.k, rs.size());
        if (srt.size() < m) sbad_row = true;
        for (std::size_t i = 0; !sbad_row && i < m; ++i)
          if (srt[i] != rs[i]) {
            if (sbad == 0)
              std::printf("    [%s] sorted prefix diff at label %d pos %zu\n",
                          tag, r, i);
            sbad_row = true;
          }
      } else {
        if (srt.size() != rs.size()) sbad_row = true;
        for (std::size_t i = 0; !sbad_row && i < rs.size() && i < srt.size();
             ++i)
          if (srt[i] != rs[i]) {
            if (sbad == 0)
              std::printf("    [%s] sorted diff at label %d pos %zu\n", tag, r,
                          i);
            sbad_row = true;
          }
      }
      if (sbad_row) ++sbad;
    }
    const std::string t(tag);
    expect(extra == 0,
           ("F8e[" + t + "]: no core rows outside the reference CC labels").c_str());
    expect(bad == 0,
           ("F8e[" + t + "]: ccemb content identical (keys + float bits)").c_str());
    expect(sbad == 0,
           prefix_only
               ? ("F8e[" + t + "]: sorted_ccemb prefix-m identical").c_str()
               : ("F8e[" + t + "]: sorted_ccemb FULLY identical").c_str());
    if (fallbacks >= 0)
      std::printf("    [%s] fastsort fallbacks=%lld of %zu CCs\n", tag,
                  fallbacks, ref.ccemb().size());
  };

  core.set_f8_ccemb_hash(false);      // accum A (dense stamp)
  core.set_f8_ccemb_fastsort(false);  // full sort A
  core.f8e_compute_ccs_emb();
  check_ccemb("A-full", false, -1);

  core.set_f8_ccemb_hash(true);       // accum B (hash)
  core.set_f8_ccemb_fastsort(false);  // full sort A
  core.f8e_compute_ccs_emb();
  check_ccemb("Bhash-full", false, -1);

  core.set_f8_ccemb_hash(false);      // accum A
  core.set_f8_ccemb_fastsort(true);   // fast sort B (prefix-m contract)
  core.f8e_compute_ccs_emb();
  check_ccemb("A-fast", true, core.f8e_fastsort_fallbacks());
  core.set_f8_ccemb_fastsort(false);

  // ---- Etapa 6 (F8f): embycc -----------------------------------------------
  ref.compute_embs_by_ccs();  // already has dense embycc

  auto check_embycc = [&](const char* tag) {
    long bad = 0, nnz = 0, neg = 0, nonf = 0;
    std::uint64_t h = BASIS;
    for (int q = 0; q < c.n; ++q) {
      auto it = ref.embycc().find(q);
      expect(it != ref.embycc().end(), "ref embycc has row q");
      if (it == ref.embycc().end()) continue;
      const auto& refrow = it->second;
      for (const auto& [r, v] : refrow) {
        const float cv = core.embycc_get(q, r);
        std::uint32_t lb, cb; std::memcpy(&lb, &v, 4); std::memcpy(&cb, &cv, 4);
        if (lb != cb) { if (bad == 0) std::printf("    [%s] embycc diff at [%d,%d]: legacy=%.9g core=%.9g\n", tag, q, r, (double)v, (double)cv); ++bad; }
        if (cv < 0.0f) ++neg; if (!std::isfinite(cv)) ++nonf; if (cv != 0.0f) ++nnz;
        h = fnv(h, &lb, sizeof(lb));
      }
    }
    // Bit-exact simEmb(embycc[k1], embycc[k2]) for pairs used in F8g
    long long ip_bad = 0;
    for (const auto& [r, m] : ref.ccemb()) {
      if (m.size() <= 1) continue;
      int k1 = ref.sorted_ccemb()[(std::size_t)r][0];
      int k2 = ref.sorted_ccemb()[(std::size_t)r][1];
      float d = ref.sim_emb_pub(ref.embycc().at(k1), ref.embycc().at(k2));
      float c = core.sim_embycc(k1, k2);
      if (d != c) {
        if (ip_bad == 0)
          std::printf("    [%s] simEmb diff for (%d,%d): legacy=%.9g core=%.9g\n", tag, k1, k2, (double)d, (double)c);
        ++ip_bad;
      }
    }
    expect(bad == 0, ("F8f[" + std::string(tag) + "]: embycc values bit-identical").c_str());
    expect(neg == 0, ("F8f[" + std::string(tag) + "]: no negative values (H16)").c_str());
    expect(nonf == 0, ("F8f[" + std::string(tag) + "]: all finite (H16)").c_str());
    expect(ip_bad == 0, ("F8f[" + std::string(tag) + "]: simEmb(embycc) bit-identical").c_str());
    std::printf("    [%s] embycc nnz=%lld neg=%lld nonf=%lld digest=0x%016llx\n", tag, nnz, neg, nonf, (unsigned long long)h);
  };

  core.set_f8_embycc_sparse(false);  // Variant A (dense) - not fully implemented, skip for now
  // core.f8f_compute_embs_by_ccs();
  // check_embycc("A-dense");

  core.set_f8_embycc_sparse(true);   // Variant B (CSR Gustavson)
  core.f8f_compute_embs_by_ccs();
  check_embycc("B-csr");
}

}  // namespace

int main() {
  const Case cases[] = {
      {"cc_structured", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_structured", 500, 100, 10, 2, 0.1, 0.05},
      {"cc_structured", 500, 100, 10, 2, 0.1, 0.5},
      {"cc_structured", 500, 100, 10, 2, 0.1, 1.0},
      {"cc_structured", 500, 100, 10, 2, 0.1, 2.0},  // th>1: whole vector
      {"cc_degenerate", 200, 40, 10, 2, 0.1, 0.0},
      {"cc_k2", 500, 100, 2, 2, 0.1, 0.0},
      {"cc_k20", 500, 100, 20, 2, 0.1, 0.0},
      {"cc_k100", 500, 100, 100, 2, 0.1, 0.0},
      // H19 self-displaced fixtures (validate accepts; every stage must match)
      {"cc_selfoff_early", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_selfoff_late", 500, 100, 10, 2, 0.1, 0.0},
      {"cc_selfoff_mixed", 500, 100, 10, 2, 0.1, 0.5},
      {nullptr, 26, 5, 5, 2, 0.1, 1.0 / 3.0},  // fractional bound (H11 shape)
      {nullptr, 26, 5, 5, 2, 0.1, 0.0},        // auto rule on synthetic rows
  };
  for (const auto& c : cases) run_case(c);
  if (g_fail) {
    std::printf("RFE CC PARITY (Etapas 2-6): %d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("RFE CC PARITY (Etapas 2-6): OK (bit-identical to the legacy)\n");
  return 0;
}