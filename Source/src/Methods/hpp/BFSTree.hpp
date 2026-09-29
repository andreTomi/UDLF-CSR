/**
 * @file BFSTree.hpp
 * @brief BFSTree on the CSRMatrix engine -- carrier A4, registered in the
 *        factory. The pipeline lives in BFSTreeCore.hpp (bit-exact port of the
 *        legacy F1..F9; provenance and contract documented there).
 *
 * Legacy parity (repo-root BFSTree.cpp, verbatim): runUdlMethod() is
 *   computeRankNormalization(1) -> (2) -> computeRankCorrelation ->
 *   initSparseMatrix(matrix) -> acumBFSTree -> normalizeWProb ->
 *   computeWDiffusion -> normalizeWProb -> execSortRankedLists
 * which is exactly the core's run(): front() = F1/F2/F3/F4(zero),
 * f5() = acumBFSTree, f6() = the colsum half of the first normalizeWProb (the
 * division is fused into the F7 read -- the F6 elimination of Etapa 3),
 * f7() = computeWDiffusion, f8() = the second normalizeWProb, f9() = the sort.
 *
 * BUILD CONTRACT -- fail-closed by design. The identity guarantees hold only
 * under the strict build (no -ffast-math, no FMA contraction). Measured under
 * the fast-math product build (dossie numerics_prod, "Passo 0"): ~89% of W
 * cells shift by 1 ULP, which flips the downstream prune frontier and changes
 * ~81% of the final rank lists. Therefore:
 *   * compiling this header under fast-math emits a build-time warning (the
 *     product still links -- failing the whole build would break every other
 *     method -- but the method refuses to run);
 *   * at run time preprocess() THROWS unless this TU was compiled without
 *     fast-math. The strict target is `make udlf-strict` -> bin/udlf_strict;
 *   * BFSTREE_ALLOW_FASTMATH=1 downgrades the refusal to a loud log line, and
 *     exists only to reproduce the divergence deliberately.
 * Residual gap (declared, not hidden): FMA contraction cannot be detected by a
 * compile-time macro, and a fast-math-free build with -ffp-contract=fast can
 * still flip bits. The strict target sets -ffp-contract=off, and the parity
 * gate (Source/tests/test_bfstree_parity.cpp) proves the flags in use reproduce
 * the legacy.
 *
 * COVERAGE (declared): the carrier keeps the legacy N^2 dense scratch for F1/F2
 * and for F5-W, so it covers the datasets where that fits in RAM (ALOI at
 * N=72k needs ~21 GB). vegfru/sun397 at full N (160k/109k -> 103/47 GB) stay
 * OUT OF REACH until the A5 carrier (W by value) is resumed -- see
 * Data/phase_profile/bfstree_a5/.
 *
 * Config contract (Validator + Test/config/*.ini): PARAM_BFSTREE_L (working
 * stride = the dataset rank-list width), PARAM_BFSTREE_K, PARAM_BFSTREE_B
 * (F5/F7 batch of rows; measured table: 256 for L=1400, 1024 for L=100),
 * PARAM_BFSTREE_CORRELATION_METRIC (only "RBO" is ported; the legacy also has
 * INTERSECTION/KENDALL_TAU/KENDALL_TAU_W/JACCARD/JACCARD_K -- asking for any of
 * those throws instead of silently running RBO).
 */
// Guard differs from the legacy header at the repo root (also BFSTree_HPP) so a
// TU that ever sees both still compiles each one.
#ifndef BFSTREE_METHOD_HPP
#define BFSTREE_METHOD_HPP

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "BFSTreeCore.hpp"
#include "Method.hpp"
#include "MethodFactory.hpp"

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#pragma message("BFSTree: TU compiled with fast-math -- BFSTree will REFUSE to run at runtime; use the strict target (make udlf-strict -> bin/udlf_strict)")
#endif

class BFSTree : public Method {
 public:
  explicit BFSTree() : Method() {}

  // Test seam: lets check_bfstree_guard.cpp exercise the refusal without a
  // full framework run (config + dataset + singletons). Called by the gates:
  // strict build -> returns normally; fast-math build -> throws.
  static void requireStrictBuildForTesting() { require_strict_build(); }

 protected:
  // ---------------------------------------------------------------- config --
  void preprocess() override {
    require_strict_build();

    l = static_cast<uint32_t>(config.get<int>("PARAM_BFSTREE_L"));
    k = config.get<int>("PARAM_BFSTREE_K");
    b = static_cast<long>(config.get<int>("PARAM_BFSTREE_B"));
    correlationMetric =
        config.get<std::string>("PARAM_BFSTREE_CORRELATION_METRIC");
    topK = k;

    if (correlationMetric != "RBO") {
      throw std::runtime_error(
          "BFSTree: correlation metric '" + correlationMetric +
          "' is not ported (only RBO is; see BFSTree.hpp)");
    }
    if (l > n) {
      throw std::runtime_error(
          "BFSTree: PARAM_BFSTREE_L can't be greater than SIZE_DATASET");
    }
    if (static_cast<int>(l) < k) {
      throw std::runtime_error("BFSTree: need L >= k");
    }
    if (b < 1) {
      throw std::runtime_error("BFSTree: PARAM_BFSTREE_B must be >= 1");
    }
    // rkLists stride == l (no L_MULT in this method); the loader is
    // line-aware, so the file must be at least this wide.
    dataProvider.setL(l);

    logMessage("[BFSTree] Initialized (A4 carrier, strict build required)");
  }

  void execute() override {
    const size_t stride = static_cast<size_t>(l);
    const size_t total = static_cast<size_t>(n) * stride;
    if (static_cast<size_t>(rkLists->size()) != total) {
      throw std::runtime_error(
          "BFSTree: rkLists width (" +
          std::to_string(rkLists->size() / static_cast<size_t>(n)) +
          ") != PARAM_BFSTREE_L (" + std::to_string(l) +
          "); check the input file width");
    }

    std::vector<int> rk(total);
    for (size_t i = 0; i < total; ++i) rk[i] = static_cast<int>((*rkLists)[i]);

    BFSTreeCore core(rk, static_cast<size_t>(n), stride, k, b);
    core.run();

    for (size_t i = 0; i < total; ++i)
      (*rkLists)[i] = static_cast<uint32_t>(rk[i]);

    logMessage("[BFSTree] A4 carrier finished (W nnz=" +
               std::to_string(core.nnz_w()) + ", W dense=" +
               std::to_string(core.dense_bytes() / (1024 * 1024)) +
               " MB, rc CSR=" +
               std::to_string(BFSTreeCore::csr_bytes(core.rc()) / (1024 * 1024)) +
               " MB)");
  }

  void postprocess() override {}

 private:
  // Refusal gate: the strict build is a precondition, not a preference. The env
  // escape hatch exists only to reproduce the fast-math divergence on purpose.
  static void require_strict_build() {
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
    if (std::getenv("BFSTREE_ALLOW_FASTMATH") != nullptr) return;
    throw std::runtime_error(
        "BFSTree: this binary was compiled with -ffast-math and cannot honor "
        "the bit-exactness contract (measured: ~89% of W cells shift by 1 ULP, "
        "~81% of the final rank lists change -- dossie numerics_prod, "
        "'Passo 0'). Use the strict build: bin/udlf_strict (make udlf-strict). "
        "Set BFSTREE_ALLOW_FASTMATH=1 only to reproduce that divergence.");
#endif
  }

  uint32_t l = 0;
  int k = 0;
  long b = 256;
  int topK = 0;
  std::string correlationMetric;
};

#endif  // BFSTREE_METHOD_HPP