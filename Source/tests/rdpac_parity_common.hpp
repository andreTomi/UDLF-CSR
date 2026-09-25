// rdpac_parity_common.hpp
//
// Shared machinery for the RDPAC phase-parity gates: embeds the frozen
// verbatim reference (test_rdpac_ref.cpp) and the sparse core, runs both on
// THE SAME input rkLists, and compares every dumped phase bit-exactly.
//
// Build (WSL, repo root; both kernels in one TU, strict IEEE):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp
//       -I Source/src -I Source/src/Core/hpp -I Source/tests
//       <test>.cpp -o /tmp/<bin>
#ifndef RDPAC_PARITY_COMMON_HPP
#define RDPAC_PARITY_COMMON_HPP

#define RDPAC_REF_AS_HEADER
#include "test_rdpac_ref.cpp"
#include "Methods/hpp/RDPACSparseCore.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

inline int g_checks = 0;
inline int g_failures = 0;

struct ParitySnap {
  std::vector<int> rk_post_fill, rk_final;
  std::vector<float> matrix_post_fill, rwl_post_fill;
  std::vector<float> w_k, rwl_post_norm, rwl_post_iter0, rwl_post_15iter;
  std::vector<float> w_post_mult1, w_post_mult2;
  uint64_t final_checksum = 0;
};

inline uint64_t rdpac_fnv_rk(const std::vector<int>& rk, size_t n,
                             size_t width) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < width; ++j) {
      h ^= static_cast<uint64_t>(static_cast<uint32_t>(rk[i * width + j]));
      h *= 1099511628211ULL;
    }
  return h;
}

inline std::vector<int> rdpac_flatten(
    const std::vector<std::vector<int>>& v) {
  std::vector<int> out;
  for (const auto& row : v) out.insert(out.end(), row.begin(), row.end());
  return out;
}

inline void rdpac_capture(const char* phase, const RDPACSparseCore& c,
                          ParitySnap& s) {
  const std::string p(phase);
  const size_t n = c.n(), width = c.width(), l = c.l();
  if (p == "post_fill") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j) {
        const int col = c.rk(i, j);
        s.rk_post_fill.push_back(col);
        s.matrix_post_fill.push_back(c.fill(i, static_cast<size_t>(col)));
        s.rwl_post_fill.push_back(c.rwl(i, j));
      }
  } else if (p == "w_k") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j <= static_cast<size_t>(c.k_end()); ++j)
        s.w_k.push_back(c.w(i, j));
  } else if (p == "rwl_post_norm") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < l; ++j) s.rwl_post_norm.push_back(c.rwl(i, j));
  } else if (p == "post_iter0") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j)
        s.rwl_post_iter0.push_back(c.rwl(i, j));
  } else if (p == "post_15iter") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j)
        s.rwl_post_15iter.push_back(c.rwl(i, j));
  } else if (p == "post_mult1") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j)
        s.w_post_mult1.push_back(c.prod(i, j));
  } else if (p == "post_mult2") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j)
        s.w_post_mult2.push_back(c.prod(i, j));
  } else if (p == "final") {
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < width; ++j) s.rk_final.push_back(c.rk(i, j));
    s.final_checksum = rdpac_fnv_rk(s.rk_final, n, width);
  }
}

inline void cmp_f(const char* name, const std::vector<float>& got,
                  const std::vector<float>& want) {
  ++g_checks;
  if (got.size() != want.size()) {
    std::printf("  [FAIL] %-18s size %zu != %zu\n", name, got.size(),
                want.size());
    ++g_failures;
    return;
  }
  long bad = -1;
  double maxd = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    uint32_t gb, wb;
    std::memcpy(&gb, &got[i], 4);
    std::memcpy(&wb, &want[i], 4);
    if (gb != wb && bad < 0) bad = static_cast<long>(i);
    if (std::isfinite(got[i]) && std::isfinite(want[i])) {
      const double d = std::fabs(static_cast<double>(got[i]) -
                                 static_cast<double>(want[i]));
      if (d > maxd) maxd = d;
    }
  }
  if (bad >= 0) {
    const size_t i = static_cast<size_t>(bad);
    std::printf("  [FAIL] %-18s first bit diff @%zu: got %.9g want %.9g\n",
                name, i, got[i], want[i]);
    ++g_failures;
    return;
  }
  std::printf("  [OK]   %-18s n=%zu max|d|=%.3g\n", name, got.size(), maxd);
}

inline void cmp_i(const char* name, const std::vector<int>& got,
                  const std::vector<int>& want) {
  ++g_checks;
  if (got.size() != want.size()) {
    std::printf("  [FAIL] %-18s size %zu != %zu\n", name, got.size(),
                want.size());
    ++g_failures;
    return;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != want[i]) {
      std::printf("  [FAIL] %-18s first diff @%zu: got %d want %d\n", name, i,
                  got[i], want[i]);
      ++g_failures;
      return;
    }
  }
  std::printf("  [OK]   %-18s n=%zu exact\n", name, got.size());
}

// Runs the frozen reference and the sparse core on the SAME input rkLists
// and compares all S1-S5 phase dumps bitwise. Failures accumulate in
// g_failures. k_inc is applied to BOTH sides (the harness global K_INC is
// overridden after allocate_structures, which resets it to 1).
inline void run_parity_case(const std::vector<int>& input, int n, int l,
                            int k_start, int k_inc, int k_end,
                            const char* label, bool expect_changed = true) {
  const int width = rdpac_ref::L_MULT * l;
  if (static_cast<int>(input.size()) != n * width)
    throw std::runtime_error("run_parity_case: input size mismatch");
  if (k_inc <= 0)
    throw std::runtime_error("run_parity_case: k_inc must be positive");

  std::printf("== Case %s (N=%d L=%d width=%d k=%d..%d step %d)\n", label, n,
              l, width, k_start, k_end, k_inc);

  // Reference side.
  rdpac_ref::allocate_structures(n, l, k_start, k_end);
  rdpac_ref::K_INC = k_inc;
  rdpac_ref::rkLists = input;
  const uint64_t init_ck = rdpac_ref::compute_rk_checksum();
  const auto dumps = rdpac_ref::run_reference_pipeline();

  // Core side (fresh copy: the reference mutated its own vector).
  std::vector<int> rk_core = input;
  RDPACSparseCore core(rk_core, static_cast<size_t>(n), static_cast<size_t>(l),
                       static_cast<size_t>(width), rdpac_ref::P_D,
                       rdpac_ref::PL, k_start, k_inc, k_end);
  ParitySnap snap;
  core.run([&](const char* ph) { rdpac_capture(ph, core, snap); });

  cmp_i("S1 rk_post_fill", snap.rk_post_fill,
        rdpac_flatten(dumps.rkLists_post_fill));
  cmp_f("S1 matrix_post_fill", snap.matrix_post_fill,
        dumps.matrix_post_fill);
  cmp_f("S1 rwl_post_fill", snap.rwl_post_fill, dumps.rwl_post_fill);
  cmp_f("S2 w_k", snap.w_k, dumps.w_k15);
  cmp_f("S2 rwl_post_norm", snap.rwl_post_norm, dumps.rwl_post_norm);
  cmp_f("S3 rwl_post_iter0", snap.rwl_post_iter0, dumps.rwl_post_iter0);
  cmp_f("S4 rwl_post_15iter", snap.rwl_post_15iter, dumps.rwl_post_15iter);
  cmp_f("S5 w_post_mult1", snap.w_post_mult1, dumps.w_post_mult1);
  cmp_f("S5 w_post_mult2", snap.w_post_mult2, dumps.w_post_mult2);
  cmp_i("S5 rk_final", snap.rk_final, rdpac_flatten(dumps.rkLists_final));

  ++g_checks;
  if (snap.final_checksum != dumps.final_rk_checksum) {
    std::printf("  [FAIL] checksum          got 0x%016llx want 0x%016llx\n",
                static_cast<unsigned long long>(snap.final_checksum),
                static_cast<unsigned long long>(dumps.final_rk_checksum));
    ++g_failures;
  } else {
    std::printf("  [OK]   checksum          0x%016llx\n",
                static_cast<unsigned long long>(snap.final_checksum));
  }

  if (expect_changed) {
    ++g_checks;
    if (snap.final_checksum == 0 || snap.final_checksum == init_ck) {
      std::printf("  [FAIL] final checksum not distinct from initial\n");
      ++g_failures;
    }
  }
}

inline int rdpac_result() {
  std::printf("RESULT: %d checks, %d failures\n", g_checks, g_failures);
  rdpac_ref::free_structures();
  return g_failures == 0 ? 0 : 1;
}

#endif  // RDPAC_PARITY_COMMON_HPP

