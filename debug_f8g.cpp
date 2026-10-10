// debug_f8g.cpp -- debug F8g cell [2,2] computation (core side only)
#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <map>

std::vector<int> load_bin(const std::string& path, std::size_t expect_n) {
  std::vector<int> rk(expect_n, 0);
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::printf("FAIL: cannot open %s\n", path.c_str()); return rk; }
  f.read(reinterpret_cast<char*>(rk.data()), (std::streamsize)(expect_n * sizeof(int)));
  return rk;
}

// Replicate reference's simEmb for maps
float ref_simEmb(const std::map<int, float>& di, const std::map<int, float>& dj) {
    float sum = 0.0f;
    auto it1 = di.begin(), it2 = dj.begin();
    while (it1 != di.end() && it2 != dj.end()) {
        if (it1->first < it2->first) ++it1;
        else if (it2->first < it1->first) ++it2;
        else { sum += it1->second * it2->second; ++it1; ++it2; }
    }
    return sum;
}

int main() {
  const int n = 500, l = 100, k = 10, t = 2;
  const double pa = 0.1, th = 0.0;
  
  std::vector<int> rk = load_bin("Data/fixtures/cc/cc_structured.bin", (std::size_t)n * l);
  
  // Run core through F7b
  RFECore core(rk, n, l, k, t, pa);
  core.f1_rank_norm();
  for (int ct = 0; ct < t; ++ct) {
    core.f2_create_emb(); core.f3_create_reverse_emb(); core.f4_combine_reverse_emb();
    core.f7a_rerank_by_emb_sim();
  }
  core.f2_create_emb(); core.f3_create_reverse_emb(); core.f4_combine_reverse_emb();
  core.f5_sort_emb(); core.f5_effect_estimation_emb();
  core.f6_gram_and_inc_elems();
  core.f7b_rerank_cprr_tail();
  
  // F8a-F8f
  core.f8a_create_edges_ccs();
  core.f8b_init_th_cc();
  core.f8c_edge_prefix();
  core.f8d_process_ccs();
  core.f8e_compute_ccs_emb();
  core.f8f_compute_embs_by_ccs();
  
  // Debug: check position_of(2, 2)
  core.build_position_index();
  unsigned int pos = core.position_of(2, 2);
  std::printf("position_of(2, 2) = %u (l+2=%d)\n", pos, l + 2);
  
  // Debug: check sim_embycc(2, 2)
  float sim = core.sim_embycc(2, 2);
  std::printf("core sim_embycc(2, 2) = %.9g\n", (double)sim);
  
  // Debug: which CCs have 2 in top-k?
  const auto& ccemb_rows = core.ccemb_rows();
  const auto& sorted_ccemb = core.sorted_ccemb();
  for (int r = 0; r < n; ++r) {
    if (ccemb_rows[(std::size_t)r].empty()) continue;
    const auto& sc = sorted_ccemb[(std::size_t)r];
    const int lim = std::min(k, (int)sc.size());
    for (int i = 0; i < lim; ++i) {
      if (sc[(std::size_t)i] == 2) {
        std::printf("CC r=%d: 2 at index i=%d\n", r, i);
      }
    }
  }
  
  // Debug: print CSR row for k=2
  const auto& sp = core.emb_by_cc_sparse();
  std::printf("CSR row 2: nnz=%zu\n", (size_t)(sp.row_ptr[3] - sp.row_ptr[2]));
  for (size_t idx = sp.row_ptr[2]; idx < sp.row_ptr[3]; ++idx) {
    std::printf("  col=%d val=%.9g\n", sp.col_idx[idx], sp.values[idx]);
  }
  
  // Reference
  RFERef ref(n, l, k, t, pa);
  ref.load_rk(rk);
  ref.set_th_cc(th);
  ref.run();
  ref.create_edges_ccs();
  ref.process_ccs();
  ref.compute_ccs_emb();
  ref.compute_embs_by_ccs();
  ref.reranking_by_ccs();
  
  // Print reference's embycc_[2] map
  const auto& ref_embycc = ref.embycc();
  std::printf("Ref embycc[2] map: size=%zu\n", ref_embycc.at(2).size());
  for (const auto& kv : ref_embycc.at(2)) {
    std::printf("  key=%d val=%.9g\n", kv.first, (double)kv.second);
  }
  
  // Compute reference's simEmb(embycc_[2], embycc_[2])
  float ref_sim = ref_simEmb(ref_embycc.at(2), ref_embycc.at(2));
  std::printf("ref simEmb(embycc[2], embycc[2]) = %.9g\n", (double)ref_sim);
  std::printf("ref simEmb(embycc_[2], embycc_[2]) = %.9g\n", (double)ref_sim);
  
  // Now run F8g and check cell [2,2]
  core.f8g_rerank_cells();
  float val = core.f8g_cell(2, 2);
  std::printf("core f8g_cell(2, 2) = %.9g\n", (double)val);
  
  const auto& rmat = ref.matrix();
  float ref_val = rmat[(std::size_t)2 * n + 2];
  std::printf("ref matrix[2,2] = %.9g\n", (double)ref_val);
  
  return 0;
}