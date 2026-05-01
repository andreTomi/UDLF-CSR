#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// We assume a configuration value is stored in a variant.
using ValueType = std::variant<int, double, bool, std::string>;

typedef struct EvalConfig {
  bool efficiencyEval;
  bool effectivenessEval;
  bool computePrecisions;
  bool computeMap;
  bool computeRecall;
  std::vector<int> recallsToCompute;
  std::vector<int> precisionsToCompute;
} EvalConfig;

typedef struct InputConfig {
  std::string fileFormat;
  std::string matrixType;
  std::string rkFormat;
  std::string sortingMethod;
  int numFusionFiles;
  std::vector<std::string> fusionFiles;
  std::string inputFile;
  std::string inputFileList;
  std::string inputFileClasses;
  std::string inputImagesPath;
} InputConfig;

typedef struct OutputConfig {
  bool outputFile;
  std::string fileFormat;
  std::string matrixType;
  std::string rkFormat;
  std::string filePath;
  int htmlPerFile;
  int htmlSize;
  bool htmlColors;
  bool htmlBeforeAfter;
  std::string logFilePath;
} OutputConfig;

typedef struct MethodConfig {
  std::map<std::string, std::variant<int, double, bool, std::string>> params;
} MethodConfig;

class Config {
 public:
  // Global parameters
  std::string task;
  std::string method;
  int datasetSize;

  std::unique_ptr<InputConfig> input;
  std::unique_ptr<OutputConfig> output;
  std::unique_ptr<EvalConfig> evaluation;
  std::unique_ptr<MethodConfig> methodParams;

  // Template to search for a configuration variable.
  // It checks global values, then input, output, evaluation, and method-specific parameters.
  template <typename T>
  T get(const std::string &key) const {
    // Global group
    if (key == "UDL_TASK") {
      if constexpr (std::is_same_v<T, std::string>)
        return task;
      else
        throw std::runtime_error("Type mismatch for key UDL_TASK: expected string.");
    }
    if (key == "UDL_METHOD") {
      if constexpr (std::is_same_v<T, std::string>)
        return method;
      else
        throw std::runtime_error("Type mismatch for key UDL_METHOD: expected string.");
    }
    if (key == "SIZE_DATASET") {
      if constexpr (std::is_same_v<T, int>)
        return datasetSize;
      else
        throw std::runtime_error("Type mismatch for key SIZE_DATASET: expected int.");
    }

    // Input group
    if (key == "INPUT_FILE") {
      if constexpr (std::is_same_v<T, std::string>)
        return input->inputFile;
      else
        throw std::runtime_error("Type mismatch for key INPUT_FILE: expected string.");
    }
    if (key == "INPUT_FILE_FORMAT") {
      if constexpr (std::is_same_v<T, std::string>)
        return input->fileFormat;
      else
        throw std::runtime_error("Type mismatch for key INPUT_FILE_FORMAT: expected string.");
    }
    if (key == "INPUT_MATRIX_TYPE") {
      if constexpr (std::is_same_v<T, std::string>)
        return input->matrixType;
      else
        throw std::runtime_error("Type mismatch for key INPUT_MATRIX_TYPE: expected string.");
    }
    if (key == "INPUT_RK_FORMAT") {
      if constexpr (std::is_same_v<T, std::string>)
        return input->rkFormat;
      else
        throw std::runtime_error("Type mismatch for key INPUT_RK_FORMAT: expected string.");
    }
    if (key == "MATRIX_TO_RK_SORTING") {
      if constexpr (std::is_same_v<T, std::string>)
        return input->sortingMethod;
      else
        throw std::runtime_error("Type mismatch for key MATRIX_TO_RK_SORTING: expected string.");
    }

    // Output group
    if (key == "OUTPUT_FILE") {
      if constexpr (std::is_same_v<T, bool>)
        return output->outputFile;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_FILE: expected bool.");
    }
    if (key == "OUTPUT_FILE_FORMAT") {
      if constexpr (std::is_same_v<T, std::string>)
        return output->fileFormat;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_FILE_FORMAT: expected string.");
    }
    if (key == "OUTPUT_MATRIX_TYPE") {
      if constexpr (std::is_same_v<T, std::string>)
        return output->matrixType;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_MATRIX_TYPE: expected string.");
    }
    if (key == "OUTPUT_RK_FORMAT") {
      if constexpr (std::is_same_v<T, std::string>)
        return output->rkFormat;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_RK_FORMAT: expected string.");
    }
    if (key == "OUTPUT_FILE_PATH") {
      if constexpr (std::is_same_v<T, std::string>)
        return output->filePath;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_FILE_PATH: expected string.");
    }
    if (key == "OUTPUT_HTML_RK_PER_FILE") {
      if constexpr (std::is_same_v<T, int>)
        return output->htmlPerFile;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_HTML_RK_PER_FILE: expected int.");
    }
    if (key == "OUTPUT_HTML_RK_SIZE") {
      if constexpr (std::is_same_v<T, int>)
        return output->htmlSize;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_HTML_RK_SIZE: expected int.");
    }
    if (key == "OUTPUT_HTML_RK_COLORS") {
      if constexpr (std::is_same_v<T, bool>)
        return output->htmlColors;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_HTML_RK_COLORS: expected bool.");
    }
    if (key == "OUTPUT_HTML_RK_BEFORE_AFTER") {
      if constexpr (std::is_same_v<T, bool>)
        return output->htmlBeforeAfter;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_HTML_RK_BEFORE_AFTER: expected bool.");
    }
    if (key == "OUTPUT_LOG_FILE_PATH") {
      if constexpr (std::is_same_v<T, std::string>)
        return output->logFilePath;
      else
        throw std::runtime_error("Type mismatch for key OUTPUT_LOG_FILE_PATH: expected string.");
    }

    // Evaluation group
    if (key == "EFFICIENCY_EVAL") {
      if constexpr (std::is_same_v<T, bool>)
        return evaluation->efficiencyEval;
      else
        throw std::runtime_error("Type mismatch for key EFFICIENCY_EVAL: expected bool.");
    }
    if (key == "EFFECTIVENESS_EVAL") {
      if constexpr (std::is_same_v<T, bool>)
        return evaluation->effectivenessEval;
      else
        throw std::runtime_error("Type mismatch for key EFFECTIVENESS_EVAL: expected bool.");
    }
    if (key == "EFFECTIVENESS_COMPUTE_PRECISIONS") {
      if constexpr (std::is_same_v<T, bool>)
        return evaluation->computePrecisions;
      else
        throw std::runtime_error("Type mismatch for key EFFECTIVENESS_COMPUTE_PRECISIONS: expected bool.");
    }
    if (key == "EFFECTIVENESS_COMPUTE_MAP") {
      if constexpr (std::is_same_v<T, bool>)
        return evaluation->computeMap;
      else
        throw std::runtime_error("Type mismatch for key EFFECTIVENESS_COMPUTE_MAP: expected bool.");
    }
    if (key == "EFFECTIVENESS_COMPUTE_RECALL") {
      if constexpr (std::is_same_v<T, bool>)
        return evaluation->computeRecall;
      else
        throw std::runtime_error("Type mismatch for key EFFECTIVENESS_COMPUTE_RECALL: expected bool.");
    }

    // Method-specific parameters: keys starting with \"PARAM_\" belong to methodParams.
    if (key.rfind("PARAM_", 0) == 0) {
      auto it = methodParams->params.find(key);
      if (it != methodParams->params.end()) {
        try {
          return std::get<T>(it->second);
        } catch (const std::bad_variant_access &) {
          throw std::runtime_error("Type mismatch for variable: " + key);
        }
      } else {
        throw std::runtime_error("Missing method parameter: " + key);
      }
    }

    throw std::runtime_error("Unknown config variable: " + key);
  }
};

#endif  // CONFIG_HPP
