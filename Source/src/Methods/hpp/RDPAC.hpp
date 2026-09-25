/**
 * @file RDPAC.hpp
 * @brief RDPAC (Rank-based Diffusion Process with Assured Convergence) on the
 *        CSRMatrix engine -- sparse reformulation of the legacy method.
 *
 * Paper: Valem, Latecki, Pedronette -- "Efficient Rank-based Diffusion Process
 * with Assured Convergence", Journal of Imaging 7(3):49, 2021.
 *
 * The verbatim reference implementation lives at the repo root (RDPAC.cpp /
 * RDPAC.hpp) and stays untouched; this port keeps its exact semantics:
 *   - rkLists working width = L_MULT * L (upstream "2l"): the first L
 *     positions receive the pl-decay fill; the full width takes part in the
 *     sorts, sumIdentityAlpha and multiplyL1;
 *   - the only dense structures were W (k+1 entries/row) and RWL (<= L
 *     entries/row) -- in CSR those are O(N*L), so the port never materializes
 *     the two N^2 arrays (41.5 GB on ALOI) the original held.
 *
 * Config contract (Validator + Test/config/*.ini): PARAM_RDPAC_{L, L_MULT, P,
 * PL, K_START, K_INC, K_END}. The framework loader is line-aware, so
 * L_MULT * PARAM_RDPAC_L must be <= the dataset rkLists width (ALOI: 400 =
 * 2 * 200); PARAM_RDPAC_L is configured as L_dataset / L_MULT.
 */
// Guard differs from the reference file at the repo root (also RDPAC_HPP) so
// a TU that ever sees both still compiles each one.
#ifndef RDPAC_METHOD_HPP
#define RDPAC_METHOD_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Method.hpp"
#include "MethodFactory.hpp"
#include "RDPACSparseCore.hpp"

class RDPAC : public Method {
 public:
  explicit RDPAC() : Method() {}

 protected:
  void preprocess() override {
    l = static_cast<uint32_t>(config.get<int>("PARAM_RDPAC_L"));
    lMult = config.get<double>("PARAM_RDPAC_L_MULT");
    p = config.get<double>("PARAM_RDPAC_P");
    pl = config.get<double>("PARAM_RDPAC_PL");
    kStart = config.get<int>("PARAM_RDPAC_K_START");
    kInc = config.get<int>("PARAM_RDPAC_K_INC");
    kEnd = config.get<int>("PARAM_RDPAC_K_END");

    // Upstream checkParameters (exit(1) -> throw, CprrSparse style).
    if (l > n) {
      throw std::runtime_error("PARAM_RDPAC_L can't be greater than SIZE_DATASET");
    }
    // Upstream initDataStructuresUdl clamp: L_MULT*L > N -> L = N / L_MULT.
    if (lMult * static_cast<double>(l) > static_cast<double>(n)) {
      l = static_cast<uint32_t>(static_cast<double>(n) / lMult);
    }
    // rkLists working width == L_MULT*L. The DataLoader writes at this stride
    // and the output width follows rkLists.size()/n, so the value must not
    // exceed the dataset file width (line-aware loader, tail would be zeros).
    listWidth = static_cast<uint32_t>(lMult * static_cast<double>(l));
    dataProvider.setL(listWidth);

    logMessage("[RDPAC] Initialized (Sparse Mode)");
  }

  void execute() override {
    // Sparse pipeline: copy the framework's VirtualArray ranked lists into
    // the core's positional vector, run the parity-validated serial kernel,
    // and copy back. The copy (N*listWidth ints, 115 MB on ALOI) is dwarfed
    // by the N^2 dense matrices the legacy code required (41.5 GB).
    const size_t stride = static_cast<size_t>(listWidth);
    const size_t total = static_cast<size_t>(n) * stride;
    if (static_cast<size_t>(rkLists->size()) != total) {
      // Line-aware loader read fewer columns than L_MULT*L: the tail would
      // hold zeros (duplicates), violating the unique-row contract.
      throw std::runtime_error(
          "RDPAC: rkLists width (" + std::to_string(rkLists->size() / n) +
          ") != PARAM_RDPAC_L_MULT * PARAM_RDPAC_L (" +
          std::to_string(stride) + "); check the input file width");
    }

    std::vector<int> rk(total);
    for (size_t i = 0; i < total; ++i) {
      rk[i] = static_cast<int>((*rkLists)[i]);
    }

    RDPACSparseCore core(rk, static_cast<size_t>(n), static_cast<size_t>(l),
                         stride, p, pl, kStart, kInc, kEnd);
    core.run();

    for (size_t i = 0; i < total; ++i) {
      (*rkLists)[i] = static_cast<uint32_t>(rk[i]);
    }
    logMessage("[RDPAC] Sparse pipeline finished (fill nnz=" +
               std::to_string(core.fill_nnz()) +
               ", nonzero=" + std::to_string(core.nonzero_count()) + ")");
  }

  void postprocess() override {}

 private:
  double lMult = 2.0;
  double p = 0.0;
  double pl = 0.0;
  int kStart = 1;
  int kInc = 1;
  int kEnd = 15;
  uint32_t listWidth = 0;

  static MethodRegister<RDPAC> signInMethod;
};

#endif  // RDPAC_METHOD_HPP
