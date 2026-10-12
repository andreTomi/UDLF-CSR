// eval_bridge.cpp -- Fase 0: validates rfe_quality.py against the REAL UDLF
// evaluation module (Evaluation.hpp + DataLoader's class-id mapping + Logger)
// in ONE run.
//
// It exercises the framework's own code path, not a re-implementation:
//   ConfigProvider::initialize -> DataLoader (LOAD_CLASSES_FILE event ->
//   loadClassesFile -> computeImageIdsAndClassSizes) -> Evaluation::evaluate
//   (assignFromTextFile (n,n) + PrecisionStrategy/RecallStrategy/MAPStrategy
//   via EventQueue) -> Logger::logEffectiveness (EFFECTIVENESS block).
//
// Two sections are logged, in order:
//   [0] "before"  = input rank list   (rk_in)
//   [1] "after"   = output rank list  (rk_out)
// The Python side (rfe_quality.py bridge) parses those blocks and compares
// them against compute_metrics on the SAME text files.
//
// MAP is computed at k = dataProvider.getL() (Evaluation.hpp:169-171), so the
// driver sets L -- i.e. MAP@L, exactly what the legacy logs in production.
//
// Usage:
//   eval_bridge <classes> <rk_in.txt> <rk_out.txt> <log_out> <N> <L>
//
// Notes:
//   * INPUT_FILE_LIST is pointed at the classes file: loadImageList fills
//     imageList, which NO evaluation strategy reads -- only its presence
//     matters (FileIO::readFileToLines must not throw).
//   * No method runs here; UDL_METHOD only feeds ConfigProvider's
//     PARAM_<method> filter. No MKL/graphblas linkage: the evaluation module
//     is header-only on top of Core/Utils.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src -I Source/src/Config/hpp
//       -I Source/src/Core/hpp -I Source/src/Utils/hpp -I Source/src/Evaluation/hpp
//       Source/tests/eval_bridge.cpp -o /tmp/eval_bridge

#include "ConfigProvider.hpp"
#include "DataLoader.hpp"
#include "Evaluation.hpp"
#include "EventQueue.hpp"
#include "Logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

int main(int argc, char** argv) {
  if (argc < 7) {
    std::printf("uso: eval_bridge <classes> <rk_in.txt> <rk_out.txt> "
                "<log_out> <N> <L>\n");
    return 2;
  }
  const std::string classes = argv[1];
  const std::string rk_in = argv[2];
  const std::string rk_out = argv[3];
  const std::string log_out = argv[4];
  const int N = std::atoi(argv[5]);
  const int L = std::atoi(argv[6]);

  using ParamTypeVariants = Type::ParamTypeVariants;
  std::map<std::string, ParamTypeVariants> cd;
  cd["UDL_TASK"] = std::string("UDL");
  cd["UDL_METHOD"] = std::string("RFE");
  cd["SIZE_DATASET"] = N;
  cd["INPUT_FILE"] = rk_in;
  cd["INPUT_FILE_LIST"] = classes;   // never read by any evaluation strategy
  cd["INPUT_FILE_CLASSES"] = classes;
  cd["INPUT_FILE_FORMAT"] = std::string("RK");
  cd["INPUT_MATRIX_TYPE"] = std::string("DIST");
  cd["INPUT_RK_FORMAT"] = std::string("NUM");
  cd["MATRIX_TO_RK_SORTING"] = std::string("HEAP");
  cd["NUM_INPUT_FUSION_FILES"] = 0;
  cd["OUTPUT_FILE"] = false;
  cd["OUTPUT_FILE_FORMAT"] = std::string("MATRIX");
  cd["OUTPUT_MATRIX_TYPE"] = std::string("DIST");
  cd["OUTPUT_RK_FORMAT"] = std::string("ALL");
  cd["OUTPUT_FILE_PATH"] = std::string("output/results.txt");
  cd["OUTPUT_HTML_RK_PER_FILE"] = 0;
  cd["OUTPUT_HTML_RK_SIZE"] = 20;
  cd["OUTPUT_HTML_RK_COLORS"] = false;
  cd["OUTPUT_HTML_RK_BEFORE_AFTER"] = false;
  cd["OUTPUT_LOG_FILE_PATH"] = log_out;
  cd["EFFICIENCY_EVAL"] = false;
  cd["EFFECTIVENESS_EVAL"] = true;
  cd["EFFECTIVENESS_COMPUTE_PRECISIONS"] = true;
  cd["EFFECTIVENESS_COMPUTE_MAP"] = true;
  cd["EFFECTIVENESS_COMPUTE_RECALL"] = true;
  cd["EFFECTIVENESS_RECALLS_TO_COMPUTE"] = std::string("5,10,20,50");
  cd["EFFECTIVENESS_PRECISIONS_TO_COMPUTE"] = std::string("5,10,20,50");

  ConfigProvider::getInstance().initialize(cd);
  const Config& config = ConfigProvider::getInstance().getConfig();

  Logger::getInstance().initialize();          // truncates log_out
  DataLoader::getInstance().initialize();      // registers LOAD_* events
  EventQueue::getInstance().processEvents();   // classes + list + rk (stride 0)

  // MAP cutoff: Evaluation.hpp:169 uses dataProvider.getL().
  DataProvider::getInstance().setL(static_cast<uint32_t>(L));

  Evaluation evaluation(config);
  evaluation.evaluate(rk_in, rk_out);  // logs "before" then "after"

  std::printf("EVALBRIDGE ok log=%s N=%d L=%d\n", log_out.c_str(), N, L);
  return 0;
}
