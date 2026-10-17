#ifndef CPRR_SPARSE_HPP
#define CPRR_SPARSE_HPP

#include "CprrSparseCore.hpp"
#include "Method.hpp"
#include "MethodFactory.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

// Serial integration of the validated sparse K1-K6 CPRR kernels.
class CprrSparse : public Method {
 public:
  explicit CprrSparse() : Method() {}

 protected:
  void preprocess() override {
    const int configuredL = config.get<int>("PARAM_CPRR_SPARSE_L");
    const int configuredK = config.get<int>("PARAM_CPRR_SPARSE_K");
    const int configuredT = config.get<int>("PARAM_CPRR_SPARSE_T");

    if (n <= 0) throw std::runtime_error("CPRR_SPARSE: N must be > 0");
    if (configuredL <= 0 || configuredK <= 0 || configuredT <= 0)
      throw std::runtime_error("CPRR_SPARSE: L, K, T must be > 0");
    l = static_cast<uint32_t>(configuredL);
    k = static_cast<uint32_t>(configuredK);
    t = static_cast<uint32_t>(configuredT);
    if (l == 0 || l > static_cast<uint32_t>(n))
      throw std::runtime_error("CPRR_SPARSE: require 1 <= L <= N");
    if (k == 0 || k > l)
      throw std::runtime_error("CPRR_SPARSE: require 1 <= K <= L");
    if (t == 0) throw std::runtime_error("CPRR_SPARSE: T must be > 0");
    if (static_cast<size_t>(n) >
        std::numeric_limits<size_t>::max() / static_cast<size_t>(l))
      throw std::runtime_error("CPRR_SPARSE: N*L overflows size_t");

    dataProvider.setL(l);
    logMessage("[CprrSparse] Initialized (serial K1-K6)");
  }

  void execute() override {
    const size_t count = static_cast<size_t>(n) * l;
    if (rkLists->size() != count)
      throw std::runtime_error("CPRR_SPARSE: input ranked-list size != N*L");

    std::vector<uint32_t> input(count);
    for (size_t i = 0; i < count; ++i) input[i] = (*rkLists)[i];
    cprr::validate_input_I4(input.data(), n, l, k, "CPRR_SPARSE");

    cprr::SparseState state = cprr::make_state(std::move(input), n, l, k, t, l);
    cprr::k1_fillPos(state);
    cprr::k2_sortZero(state);

    for (size_t iteration = 1; iteration <= state.t; ++iteration) {
      cprr::k3_cartProd(state);
      cprr::k4_reverseCartProd(state);
      cprr::k5_cleanRef(state);
      const bool clean = state.rev_src.empty() && state.rev_pos.empty() &&
                         state.rev_rowptr.size() == state.n + 1 &&
                         std::all_of(state.nRef.begin(), state.nRef.end(),
                                     [](size_t refs) { return refs == 0; });
      if (!clean)
        throw std::runtime_error("CPRR_SPARSE: K5 failed to clear reverse references");

      if (iteration == state.t)
        cprr::k6_sort(state);
      else
        cprr::k2_sortZero(state);
    }

    for (size_t i = 0; i < count; ++i) (*rkLists)[i] = state.lists[i];
  }

  void postprocess() override {}

 private:
  uint32_t k = 0;
  uint32_t t = 0;
  static MethodRegister<CprrSparse> signInMethod;
};

#endif  // CPRR_SPARSE_HPP