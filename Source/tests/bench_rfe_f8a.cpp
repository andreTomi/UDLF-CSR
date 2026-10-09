// bench_rfe_f8a.cpp -- Etapa 3 variant selection for f8a_create_edges_ccs.
//
// Times variant A (serial, q order) vs variant B (parallel q-chunks) on a
// real dataset, with 1 warmup + N reps (default 5), reporting median and min
// per mode. The pipeline up to f5 runs ONCE (f8a reads emb_/ee_/rk_ only, so
// repeated calls see identical state); a digest over the edge vector is
// printed per mode so A==B is visible in the log, not assumed.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp Source/tests/bench_rfe_f8a.cpp
//       -o /tmp/bench_rfe_f8a
// Run (threads via OMP_NUM_THREADS):
//   /tmp/bench_rfe_f8a <rk.bin> <L> <k> <t> <pa> [reps]
//   ALOI: /tmp/bench_rfe_f8a /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin 400 20 2 0.1

#include "Methods/hpp/RFECore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::uint64_t digest_edges(
    const std::vector<std::pair<float, std::pair<int, int>>>& e) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const auto& t : e) {
    std::uint32_t bits;
    std::memcpy(&bits, &t.first, 4);
    h ^= bits;
    h *= 1099511628211ULL;
    h ^= (std::uint32_t)t.second.first;
    h *= 1099511628211ULL;
    h ^= (std::uint32_t)t.second.second;
    h *= 1099511628211ULL;
  }
  return h;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::printf("usage: %s <rk.bin> <L> <k> <t> <pa> [reps]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const int L = std::atoi(argv[2]);
  const int k = std::atoi(argv[3]);
  const int t = std::atoi(argv[4]);
  const double pa = std::atof(argv[5]);
  const int reps = argc > 6 ? std::atoi(argv[6]) : 5;

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::printf("FAIL: cannot open %s\n", path);
    return 1;
  }
  const std::size_t bytes = (std::size_t)f.tellg();
  const std::size_t n = bytes / (sizeof(int) * (std::size_t)L);
  f.seekg(0);
  std::vector<int> rk(n * (std::size_t)L);
  f.read(reinterpret_cast<char*>(rk.data()), (std::streamsize)rk.size() * 4);
  if ((std::size_t)f.gcount() != rk.size() * sizeof(int)) {
    std::printf("FAIL: short read\n");
    return 1;
  }
  std::printf("F8A_BENCH path=%s N=%zu L=%d k=%d t=%d reps=%d threads=%d\n",
              path, n, L, k, t, reps, omp_get_max_threads());

  // Setup once: run()'s exact sequence WITHOUT validate_input(). H19 (see
  // FINDINGS): the ALOI production file has 552/72000 rows whose self sits
  // somewhere OTHER than index 0 (all rows do contain self), so the H4 entry
  // check refuses the file at run() start -- a precondition stricter than the
  // legacy's (f1's kernel sort finds self anywhere and PINS it to 0; every
  // later phase sees self at 0). The phase sequence below is run()'s, spelled
  // out; the CC entry's re-embed prefix follows, exactly as execRRCCs does.
  RFECore core(rk, n, (std::size_t)L, k, t, pa);
  core.f1_rank_norm();
  for (int ct = 0; ct < t; ++ct) {
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
  // execRRCCs prefix: re-embed over the post-f7b lists (f8a consumes this).
  core.f2_create_emb();
  core.f3_create_reverse_emb();
  core.f4_combine_reverse_emb();
  core.f5_sort_emb();
  core.f5_effect_estimation_emb();

  for (int mode = 0; mode <= 1; ++mode) {
    core.set_f8_edges_parallel(mode == 1);
    std::vector<double> samples;
    std::uint64_t digest = 0;
    for (int r = -1; r < reps; ++r) {  // r = -1 is the warmup
      const Clock::time_point t0 = Clock::now();
      core.f8a_create_edges_ccs();
      const double dt = ms_since(t0);
      digest = digest_edges(core.edges_ccs());
      if (r >= 0) samples.push_back(dt);
    }
    const double mn = *std::min_element(samples.begin(), samples.end());
    std::printf("F8A mode=%c E=%zu digest=0x%016llx median=%.3fms min=%.3fms\n",
                mode == 1 ? 'B' : 'A', core.edges_ccs().size(),
                (unsigned long long)digest, median(samples), mn);
  }

  // ---- F8D (Etapa 4): components, variant A (literal map/set) vs B (DSU) --
  // State: edges from the last f8a call (identical across modes by digest).
  // Serial phase by design (edge order defines labels), so threads do not
  // affect it; measured on one thread's wall time regardless of setting.
  // NOTE: th_cc_ is still 0.0 here; the FIRST f8d call initializes it through
  // f8b (auto rule), so the prefix is printed AFTER each call, never before.
  for (int mode = 0; mode <= 1; ++mode) {
    core.set_f8_components_dsu(mode == 1);
    std::vector<double> samples;
    for (int r = -1; r < reps; ++r) {  // r = -1 warmup
      const Clock::time_point t0 = Clock::now();
      core.f8d_process_ccs();
      const double dt = ms_since(t0);
      if (r >= 0) samples.push_back(dt);
    }
    std::uint64_t h = 1469598103934665603ULL;
    for (int lab : core.f8_cc_label()) {
      h ^= (std::uint32_t)lab;
      h *= 1099511628211ULL;
    }
    const double mn = *std::min_element(samples.begin(), samples.end());
    std::printf("F8D mode=%c prefix=%zu th=%.9g labels=%zu digest=0x%016llx "
                "median=%.3fms min=%.3fms\n",
                mode == 1 ? 'B' : 'A', core.f8c_edge_prefix(),
                core.th_cc(), core.f8_cc_label().size(),
                (unsigned long long)h, median(samples), mn);
  }

  // ---- F8E (Etapa 5): ccemb accumulation A (dense) vs B (hash), and ----
  // ---- full sort vs fast top-m (fallback rate reported)            ----
  core.f8e_compute_ccs_emb();  // state: labels from the last f8d
  for (int hash = 0; hash <= 1; ++hash) {
    core.set_f8_ccemb_hash(hash == 1);
    core.set_f8_ccemb_fastsort(false);
    std::vector<double> samples;
    for (int r = -1; r < reps; ++r) {
      const Clock::time_point t0 = Clock::now();
      core.f8e_compute_ccs_emb();
      const double dt = ms_since(t0);
      if (r >= 0) samples.push_back(dt);
    }
    std::uint64_t h2 = 1469598103934665603ULL;
    std::size_t rows = 0, keys = 0;
    for (const auto& row : core.ccemb_rows()) {
      if (!row.empty()) ++rows;
      keys += row.size();
      for (const auto& kv : row) {
        std::uint32_t bits;
        std::memcpy(&bits, &kv.second, 4);
        h2 ^= bits;
        h2 *= 1099511628211ULL;
        h2 ^= (std::uint32_t)kv.first;
        h2 *= 1099511628211ULL;
      }
    }
    const double mn = *std::min_element(samples.begin(), samples.end());
    std::printf("F8E accum=%c rows=%zu keys=%zu digest=0x%016llx "
                "median=%.3fms min=%.3fms\n",
                hash ? 'B' : 'A', rows, keys, (unsigned long long)h2,
                median(samples), mn);
  }
  // fast sort vs full sort (accum A), fallback rate over all CCs.
  for (int fast = 0; fast <= 1; ++fast) {
    core.set_f8_ccemb_hash(false);
    core.set_f8_ccemb_fastsort(fast == 1);
    std::vector<double> samples;
    for (int r = -1; r < reps; ++r) {
      const Clock::time_point t0 = Clock::now();
      core.f8e_compute_ccs_emb();
      const double dt = ms_since(t0);
      if (r >= 0) samples.push_back(dt);
    }
    const double mn = *std::min_element(samples.begin(), samples.end());
    std::printf("F8E sort=%c fallbacks=%lld median=%.3fms min=%.3fms\n",
                fast ? 'B' : 'A', core.f8e_fastsort_fallbacks(),
                median(samples), mn);
  }
  core.set_f8_ccemb_fastsort(false);

  // ---- F8F (Etapa 6): embycc ----
  core.f8f_compute_embs_by_ccs();
  std::vector<double> samples_f8f;
  for (int r = -1; r < reps; ++r) {
    const Clock::time_point t0 = Clock::now();
    core.f8f_compute_embs_by_ccs();
    const double dt = ms_since(t0);
    if (r >= 0) samples_f8f.push_back(dt);
  }
  std::uint64_t h2 = 1469598103934665603ULL;
  std::size_t rows = 0, keys = 0;
  for (const auto& row : core.ccemb_rows()) {
    if (!row.empty()) ++rows;
    keys += row.size();
  }
  const double mn = *std::min_element(samples_f8f.begin(), samples_f8f.end());
  std::printf("F8F median=%.3fms min=%.3fms\n", median(samples_f8f), mn);

  core.set_f8_ccemb_fastsort(false);
  return 0;
}