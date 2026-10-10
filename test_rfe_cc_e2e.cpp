// test_rfe_cc_e2e.cpp -- end-to-end CC integration test (Phase B)
// Verifies run() with performCCs=true matches reference's run()+execRRCCs()
#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

std::vector<int> load_bin(const std::string& path, std::size_t expect_n) {
  std::vector<int> rk(expect_n, 0);
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::printf("FAIL: cannot open %s\n", path.c_str()); return rk; }
  f.read(reinterpret_cast<char*>(rk.data()), (std::streamsize)(expect_n * sizeof(int)));
  return rk;
}

constexpr std::uint64_t BASIS = 1469598103934665603ULL;
std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t nb) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < nb; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

int main() {
  const int n = 500, l = 100, k = 10, t = 2;
  const double pa = 0.1, th = 0.0;
  
  // Load fixture
  std::vector<int> rk = load_bin("Data/fixtures/cc/cc_structured.bin", (std::size_t)n * l);
  
  // Reference: step through to match core's manual stepping
  RFERef ref(n, l, k, t, pa);
  ref.load_rk(rk);
  ref.set_th_cc(th);
  ref.run();
  ref.create_edges_ccs();  // this calls initEdgesCCs which sets th_cc
  std::printf("REF th_out (after create_edges_ccs) = %.9g\n", ref.th_cc());
  
  ref.process_ccs();
  ref.compute_ccs_emb();
  ref.compute_embs_by_ccs();
  ref.reranking_by_ccs();
  
  const auto& rmat = ref.matrix();
  std::uint64_t ref_rk_hash = 0;
  for (int q = 0; q < n; ++q) {
    for (int i = 0; i < l; ++i) {
      ref_rk_hash = fnv(ref_rk_hash, &ref.rkLists()[(std::size_t)q * l + i], sizeof(int));
    }
  }
  std::printf("REF rk FNV = 0x%016llx\n", ref_rk_hash);
  
  // Core: manually step through to match reference
  RFECore core(rk, n, l, k, t, pa, /*performCCs=*/true, th);
  
  // F1 + t-loop
  core.f1_rank_norm();
  for (int ct = 0; ct < t; ++ct) {
    core.f2_create_emb(); core.f3_create_reverse_emb(); core.f4_combine_reverse_emb();
    core.f7a_rerank_by_emb_sim();
  }
  
  // Post-t-loop re-embedding (like reference's doHyperRREmbedding in execCartesianProduct)
  core.f2_create_emb(); core.f3_create_reverse_emb(); core.f4_combine_reverse_emb();
  core.f5_sort_emb(); core.f5_effect_estimation_emb();
  core.f6_gram_and_inc_elems();
  core.f7b_rerank_cprr_tail();
  
  // CC path - f8a (edges only, no th_cc)
  core.f8a_create_edges_ccs();
  // f8b - this should match reference's initEdgesCCs
  core.f8b_init_th_cc();
  std::printf("CORE th_cc (before f8c) = %.9g\n", core.th_cc());
  
  // Debug: compute sim for cell (0,32) manually
  // Access private nnz_cols via a helper - skip for now
  std::printf("DEBUG: checking sim for cell (0,32)\\n");
  
  // Continue core
  core.f8c_edge_prefix();
  core.f8d_process_ccs();
  core.f8e_compute_ccs_emb();
  core.f8f_compute_embs_by_ccs();
  core.f8g_rerank_cells();
  core.f8h_rerank_tail();
  
  std::printf("CORE th_cc (final) = %.9g\n", core.th_cc());
  
  std::uint64_t core_rk_hash = 0;
  for (int q = 0; q < n; ++q) {
    for (int i = 0; i < l; ++i) {
      core_rk_hash = fnv(core_rk_hash, &core.rk_[(std::size_t)q * l + i], sizeof(int));
    }
  }
  std::printf("CORE rk FNV = 0x%016llx\n", core_rk_hash);
  
  // Reference hash already computed above
  // std::uint64_t ref_rk_hash = 0;  // Already declared above
  // for (int q = 0; q < n; ++q) {
  //   for (int i = 0; i < l; ++i) {
  //     ref_rk_hash = fnv(ref_rk_hash, &ref.rkLists()[(std::size_t)q * l + i], sizeof(int));
  //   }
  // }
  // std::printf("REF rk FNV = 0x%016llx\n", ref_rk_hash);
  
  if (core_rk_hash == ref_rk_hash) {
    std::printf("E2E CC INTEGRATION: PASS (rk identical to reference)\n");
    return 0;
  } else {
    std::printf("E2E CC INTEGRATION: FAIL (rk differs from reference)\n");
    return 1;
  }
}