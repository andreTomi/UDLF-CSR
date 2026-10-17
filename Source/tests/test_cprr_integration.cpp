// Framework-level CPRR_SPARSE integration gate. Each invocation runs one
// fixture in a fresh process because ConfigProvider/DataProvider are singletons.
#include "Config/hpp/ConfigProvider.hpp"
#include "Core/hpp/DataLoader.hpp"
#include "Methods/hpp/CprrSparse.hpp"
#include "Methods/hpp/MethodFactory.hpp"
#include "Utils/hpp/FileIO.hpp"
#include "Utils/hpp/Logger.hpp"
#include "snapshot.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// The normal application defines this registration in Method.cpp. This test
// TU links only CPRR_SPARSE so it supplies the same registration explicitly.
MethodRegister<CprrSparse> CprrSparse::signInMethod("CPRR_SPARSE");

namespace {

class DirectCprrSparse : public CprrSparse {
 public:
  void postprocess() override {}
  void runKernelThroughFrameworkInput() {
    preprocess();
    EventQueue::getInstance().processEvents();
    execute();
    postprocess();
  }
};

std::vector<uint32_t> read_output(const std::string& path, size_t n, size_t L) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read generated ranking: " + path);
  std::vector<uint32_t> values;
  values.reserve(n * L);
  uint64_t value = 0;
  while (in >> value) {
    if (value > UINT32_MAX) throw std::runtime_error("output ID exceeds uint32_t");
    values.push_back(static_cast<uint32_t>(value));
  }
  if (!in.eof()) throw std::runtime_error("malformed token in generated ranking");
  if (values.size() != n * L)
    throw std::runtime_error("generated ranking has unexpected element count");
  return values;
}

void compare(const std::vector<uint32_t>& actual, const cmp::RankMat& expected,
             size_t n, size_t L) {
  if (expected.n != n || expected.L != L || expected.v.size() != n * L)
    throw std::runtime_error("approved ranking snapshot shape mismatch");
  if (actual.size() != expected.v.size())
    throw std::runtime_error("actual ranking size mismatch");
  for (size_t i = 0; i < actual.size(); ++i) {
    if (expected.v[i] < 0 || actual[i] != static_cast<uint32_t>(expected.v[i])) {
      throw std::runtime_error("ranking mismatch at row " + std::to_string(i / L) +
                               ", position " + std::to_string(i % L) +
                               ": got " + std::to_string(actual[i]) +
                               ", expected " + std::to_string(expected.v[i]));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 10) {
      std::fprintf(stderr,
                   "usage: test_cprr_integration N L K T INPUT EXPECTED.snp WORKDIR MODE FIXTURE\n");
      return 2;
    }
    const size_t n = std::stoull(argv[1]);
    const size_t L = std::stoull(argv[2]);
    const size_t k = std::stoull(argv[3]);
    const size_t t = std::stoull(argv[4]);
    const std::string input = std::filesystem::absolute(argv[5]).string();
    const std::string expected_path = std::filesystem::absolute(argv[6]).string();
    const std::filesystem::path workdir = std::filesystem::absolute(argv[7]);
    const std::string mode = argv[8];
    const std::string fixture = argv[9];
    if (mode != "direct" && mode != "framework")
      throw std::runtime_error("mode must be direct or framework");
    std::filesystem::create_directories(workdir / "output");

    std::map<std::string, Type::ParamTypeVariants> cfg;
    cfg["UDL_TASK"] = std::string("UDL");
    cfg["UDL_METHOD"] = std::string("CPRR_SPARSE");
    cfg["SIZE_DATASET"] = static_cast<int>(n);
    cfg["INPUT_FILE"] = input;
    cfg["INPUT_FILE_LIST"] = (workdir / "images.txt").string();
    cfg["INPUT_FILE_CLASSES"] = (workdir / "classes.txt").string();
    cfg["INPUT_FILE_FORMAT"] = std::string("RK");
    cfg["INPUT_RK_FORMAT"] = std::string("NUM");
    cfg["INPUT_MATRIX_TYPE"] = std::string("SIM");
    cfg["MATRIX_TO_RK_SORTING"] = std::string("HEAP");
    cfg["NUM_INPUT_FUSION_FILES"] = 0;
    cfg["OUTPUT_FILE"] = true;
    cfg["OUTPUT_FILE_FORMAT"] = std::string("RK");
    cfg["OUTPUT_RK_FORMAT"] = std::string("NUM");
    cfg["OUTPUT_FILE_PATH"] = (workdir / "matrix.txt").string();
    cfg["OUTPUT_LOG_FILE_PATH"] = (workdir / "run.log").string();
    cfg["EFFICIENCY_EVAL"] = false;
    cfg["EFFECTIVENESS_EVAL"] = false;
    cfg["EFFECTIVENESS_COMPUTE_MAP"] = false;
    cfg["EFFECTIVENESS_COMPUTE_PRECISIONS"] = false;
    cfg["EFFECTIVENESS_COMPUTE_RECALL"] = false;
    cfg["EFFECTIVENESS_RECALLS_TO_COMPUTE"] = std::string("");
    cfg["EFFECTIVENESS_PRECISIONS_TO_COMPUTE"] = std::string("");
    cfg["PARAM_CPRR_SPARSE_L"] = static_cast<int>(L);
    cfg["PARAM_CPRR_SPARSE_K"] = static_cast<int>(k);
    cfg["PARAM_CPRR_SPARSE_T"] = static_cast<int>(t);
    ConfigProvider::getInstance().initialize(cfg);
    if (mode == "framework") DataLoader::getInstance().initialize();
    DataProvider& data = DataProvider::getInstance();
    data.setL(static_cast<uint32_t>(L));
    data.getRkLists().assignFromTextFile(input, n, L);

    const snap::Snapshot expected_snapshot = snap::loadSnapshot(expected_path);
    if (expected_snapshot.layout != snap::Layout::Ranking)
      throw std::runtime_error("approved snapshot is not a ranking");
    const cmp::RankMat expected = snap::decodeRanking(expected_snapshot);
    const std::filesystem::path old_cwd = std::filesystem::current_path();
    std::filesystem::current_path(workdir);

    if (mode == "framework") {
      Logger::getInstance().initialize();
      std::unique_ptr<Method> method = MethodFactory::createMethod("CPRR_SPARSE");
      method->run();
    } else {
      DirectCprrSparse method;
      method.runKernelThroughFrameworkInput();
      FileIO::writeRankedListsToFile((workdir / "reranked_lists.txt").string(),
                                     static_cast<int>(n));
    }

    const std::string output_path = mode == "framework"
        ? (workdir / "output/reranked_lists.txt").string()
        : (workdir / "reranked_lists.txt").string();
    const std::vector<uint32_t> actual = read_output(output_path, n, L);
    compare(actual, expected, n, L);
    std::filesystem::current_path(old_cwd);
    std::printf("CPRR_INTEGRATION,PASS,fixture=%s,n=%zu,L=%zu,k=%zu,t=%zu,mode=%s,ids=%zu\n",
                fixture.c_str(), n, L, k, t, mode.c_str(), actual.size());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "CPRR_INTEGRATION,FAIL,%s\n", e.what());
    return 1;
  }
}