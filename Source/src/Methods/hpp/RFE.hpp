/**
 * @file RFE.hpp
 * @brief RFE (Ranked-Flow Embedding) on the CSRMatrix engine -- carrier A4,
 *        registered in the factory. The pipeline lives in RFECore.hpp (bit-exact
 *        port of the legacy F1..F7b; provenance and contract documented there).
 *
 * Legacy parity (repo-root RFE.cpp, verbatim): runUdlMethod() is
 *   rankNorm() -> t x execEmbeddingRR() -> execCartesianProduct() ->
 *   (if performCCs) execRRCCs() -> (if rerankByEmb||exportEmbeddings) execRRByEmbCCs()
 * which is exactly the core's run(): f1_rank_norm() -> t x (f2,f3,f4,f7a) ->
 * f2,f3,f4 -> f5 -> f6 -> f7b (f7a = reRankingByEmbSim, the t-loop phase).
 *
 * BUILD CONTRACT -- fail-closed by design. The identity guarantees hold only
 * under the strict build (no -ffast-math, no FMA contraction). Measured under
 * the fast-math product build (dossier numerics_prod, "Passo 0"): the BFSTree
 * analogue showed ~89% of W cells shift by 1 ULP and ~81% of final rank lists
 * change. Therefore:
 *   * compiling this header under fast-math emits a build-time warning (the
 *     product still links -- failing the whole build would break every other
 *     method -- but the method refuses to run);
 *   * at run time preprocess() THROWS unless this TU was compiled without
 *     fast-math. The strict target is `make udlf-strict` -> bin/udlf_strict;
 *   * RFE_ALLOW_FASTMATH=1 downgrades the refusal to a loud log line, and
 *     exists only to reproduce the divergence deliberately.
 * Residual gap (declared, not hidden): FMA contraction cannot be detected by a
 * compile-time macro, and a fast-math-free build with -ffp-contract=fast can
 * still flip bits. The strict target sets -ffp-contract=off, and the parity
 * gate (Source/tests/test_rfe_parity.cpp) proves the flags in use reproduce
 * the legacy.
 *
 * Config contract (Validator + Test/config/*.ini): PARAM_RFE_K, PARAM_RFE_T,
 * PARAM_RFE_L, PARAM_RFE_PA, PARAM_RFE_PERFORM_CCS, PARAM_RFE_RERANK_BY_EMB,
 * PARAM_RFE_EXPORT_EMBEDDINGS, PARAM_RFE_EMBEDDINGS_PATH, PARAM_RFE_CCS_PATH,
 * PARAM_RFE_TH_CC (threshold for CC, used in execRRCCs).
 */
#ifndef RFE_METHOD_HPP
#define RFE_METHOD_HPP

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <random>
#include <cstdio>
#include <algorithm>

#include "Method.hpp"
#include "MethodFactory.hpp"
#include "RFECore.hpp"

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#pragma message("RFE: TU compiled with fast-math -- RFE will REFUSE to run at runtime; use the strict target (make udlf-strict -> bin/udlf_strict)")
#endif

class RFE : public Method {
public:
  explicit RFE() : Method() {}

  // Test seam: lets check_rfe_guard.cpp exercise the refusal without a
  // full framework run (config + dataset + singletons). Called by the gates:
  // strict build -> returns normally; fast-math build -> throws.
  static void requireStrictBuildForTesting() { require_strict_build(); }

protected:
  // Refusal gate: the strict build is a precondition, not a preference. The env
  // escape hatch exists only to reproduce the fast-math divergence on purpose.
  static void require_strict_build() {
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
    if (std::getenv("RFE_ALLOW_FASTMATH") != nullptr) return;
    throw std::runtime_error(
        "RFE: this binary was compiled with -ffast-math and cannot honor "
        "the bit-exactness contract (measured: F7a bottleneck 43-52%, "
        "pipeline relies on reassociation-sensitive FP). Use the strict build: "
        "bin/udlf_strict (make udlf-strict). "
        "Set RFE_ALLOW_FASTMATH=1 only to reproduce that divergence.");
#endif
  }

  // ---------------------------------------------------------------- config --
  void preprocess() override {
    require_strict_build();

    k = static_cast<int>(config.get<int>("PARAM_RFE_K"));
    t = config.get<int>("PARAM_RFE_T");
    l = static_cast<uint32_t>(config.get<int>("PARAM_RFE_L"));
    pa = config.get<double>("PARAM_RFE_PA");
    th_cc = config.get<double>("PARAM_RFE_TH_CC");
    performCCs = config.get<bool>("PARAM_RFE_PERFORM_CCS");

    // Legacy also has rerankByEmb / exportEmbeddings / paths -- not ported.
    if (config.get<bool>("PARAM_RFE_RERANK_BY_EMB") ||
        config.get<bool>("PARAM_RFE_EXPORT_EMBEDDINGS")) {
      throw std::runtime_error(
          "RFE: PARAM_RFE_RERANK_BY_EMB / EXPORT_EMBEDDINGS not ported");
    }

    // Contractual validations (legacy exit(1) -> throw)
    if (l > n) throw std::runtime_error("PARAM_RFE_L can't be greater than SIZE_DATASET");
    if (k > static_cast<int>(l)) throw std::runtime_error("RFE: need K <= L");
    if (k < 2) throw std::runtime_error("RFE: need K >= 2");

    // H5: Canary check -- verify strict build produces faithful ck_gram on synthetic fixture
    // This catches silent regime drift (e.g., fast-math linked in) before running on real data.
    // Uses the pinned fixture: N=200, L=40, k=10, t=2, pa=0.1, seed=42
    // Expected faithful ck_gram (post-run()): 0x46322e3d65fe6403
    {
      std::vector<int> rk = gen_fixture(42, 200, 40);
      RFECore canary(rk, 200, 40, 10, 2, 0.1);
      canary.run();
      constexpr unsigned long long FAITHFUL_CK_GRAM = 0x46322e3d65fe6403ULL;
      if (canary.ck_gram() != FAITHFUL_CK_GRAM) {
        throw std::runtime_error(
            "RFE canary FAILED: ck_gram=" + to_hex(canary.ck_gram()) +
            " expected " + to_hex(0x46322e3d65fe6403ULL) +
            " -- this binary was likely built with fast-math or wrong flags");
      }
    }

    // DataProvider stride == l (rkLists width == l, no L_MULT)
    dataProvider.setL(l);

    // PERFORM_CCS is integrated (Etapa 9), but F8g/F8h are WIP and the CC gate
    // is not green. Default is REFUSED to prevent silent divergence.
    // Use RFE_ALLOW_CC=1 to enable during development.
    logMessage("[RFE] Initialized (strict build required; PERFORM_CCS NOT honoured -- WIP, refused in run())");
  }

private:
  // Fisher-Yates generator for canary fixture (same as test_rfe_parity.cpp)
  static std::vector<int> gen_fixture(int seed, int N, int L) {
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

  static std::string to_hex(unsigned long long v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "0x%016llx", v);
    return std::string(buf);
  }

  int k = 20;
  int t = 2;
  uint32_t l = 400;
  double pa = 0.1;
  double th_cc = 0.0;
  bool performCCs = false;  // PARAM_RFE_PERFORM_CCS (legacy RFE.cpp:43)

  static MethodRegister<RFE> signInMethod;

  // ---------------------------------------------------------------- execute --
  void execute() override {
    const size_t stride = static_cast<size_t>(l);
    const size_t total = static_cast<size_t>(n) * stride;
    if (static_cast<size_t>(rkLists->size()) != total) {
      throw std::runtime_error(
          "RFE: rkLists width (" + std::to_string(rkLists->size() / n) +
          ") != PARAM_RFE_L (" + std::to_string(l) +
          "); check the input file width");
    }

    // Copy VirtualArray -> vector<int> (exactly like BFSTree/RDPAC)
    std::vector<int> rk(total);
    for (size_t i = 0; i < total; ++i)
      rk[i] = static_cast<int>((*rkLists)[i]);

    // RFECore constructor validates performCCs
    RFECore core(rk, static_cast<size_t>(n), stride, k, t, pa, performCCs, th_cc);

    // X9: carrier calls EXACTLY core.run() -- do NOT re-spell the sequence
    core.run();

    // Copy result back to VirtualArray
    for (size_t i = 0; i < total; ++i)
      (*rkLists)[i] = static_cast<uint32_t>(rk[i]);

    logMessage("[RFE] Finished (rk nnz=" + std::to_string(total) + ")");
  }

  void postprocess() override {}

}; // class RFE

#endif // RFE_METHOD_HPP