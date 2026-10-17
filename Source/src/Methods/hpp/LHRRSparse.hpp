#ifndef LHRR_SPARSE_HPP
#define LHRR_SPARSE_HPP

#include "LHRRSparseCore.hpp"
#include "Method.hpp"
#include "MethodFactory.hpp"

#include <limits>
#include <stdexcept>
#include <vector>

// Framework adapter for the validated LHRR sparse core. Parallel phase C is
// opt-in; the default remains the serial execution path.
class LHRRSparse : public Method {
 public:
  explicit LHRRSparse() : Method() {}

 protected:
  void preprocess() override {
    const int configuredL = config.get<int>("PARAM_LHRR_SPARSE_L");
    const int configuredK = config.get<int>("PARAM_LHRR_SPARSE_K");
    const int configuredT = config.get<int>("PARAM_LHRR_SPARSE_T");
    const int configuredThreads = config.get<int>("PARAM_LHRR_SPARSE_THREADS");

    if (n <= 0) throw std::runtime_error("LHRR_SPARSE: N must be > 0");
    if (configuredL <= 0 || configuredK < 2 || configuredT <= 0 ||
        configuredThreads <= 0)
      throw std::runtime_error("LHRR_SPARSE: L > 0, K >= 2, T > 0 and threads > 0 required");
    if (configuredL > n || configuredK > configuredL)
      throw std::runtime_error("LHRR_SPARSE: require 2 <= K <= L <= N");
    if (static_cast<size_t>(n) >
        std::numeric_limits<size_t>::max() / static_cast<size_t>(configuredL))
      throw std::runtime_error("LHRR_SPARSE: N*L overflows size_t");

    l = static_cast<uint32_t>(configuredL);
    dataProvider.setL(l);
    logMessage("[LHRRSparse] Initialized (faithful sparse core)");
  }

  void execute() override {
    const size_t count = static_cast<size_t>(n) * l;
    if (rkLists->size() != count)
      throw std::runtime_error("LHRR_SPARSE: input ranked-list size != N*L");

    std::vector<uint32_t> input(count);
    for (size_t i = 0; i < count; ++i) input[i] = (*rkLists)[i];

    const int k = config.get<int>("PARAM_LHRR_SPARSE_K");
    const int t = config.get<int>("PARAM_LHRR_SPARSE_T");
    const int threads = config.get<int>("PARAM_LHRR_SPARSE_THREADS");
    lhrr_sparse::Pipeline pipeline;
    pipeline.init(static_cast<int>(n), static_cast<int>(l), k, t);
    pipeline.set_rkLists(input.data());
    pipeline.run(threads);

    const auto& result = pipeline.get_result();
    for (size_t i = 0; i < count; ++i) (*rkLists)[i] = result[i];
  }

  void postprocess() override {}

 private:
  static MethodRegister<LHRRSparse> signInMethod;
};

#endif  // LHRR_SPARSE_HPP