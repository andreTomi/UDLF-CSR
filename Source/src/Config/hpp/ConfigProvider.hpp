#ifndef CONFIG_PROVIDER_HPP
#define CONFIG_PROVIDER_HPP

#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Config.hpp"
#include "Singleton.hpp"
#include "Type.hpp"

// A configuration value is stored as a variant.
using ParamTypeVariants = Type::ParamTypeVariants;

class ConfigProvider : public Singleton<ConfigProvider> {
 public:
  friend class Singleton<ConfigProvider>;

  // Initialize the ConfigProvider with configuration data.
  // This function must be called once at startup.
  void initialize(const std::map<std::string, ParamTypeVariants> &configData) {
    // General parameters.
    setConfigVariable(config.task, configData, "UDL_TASK");
    setConfigVariable(config.method, configData, "UDL_METHOD");
    setConfigVariable(config.datasetSize, configData, "SIZE_DATASET");

    // Input parameters.
    config.input = std::make_unique<InputConfig>();
    setConfigVariable(config.input->fileFormat, configData, "INPUT_FILE_FORMAT");
    setConfigVariable(config.input->matrixType, configData, "INPUT_MATRIX_TYPE");
    setConfigVariable(config.input->rkFormat, configData, "INPUT_RK_FORMAT");
    setConfigVariable(config.input->sortingMethod, configData, "MATRIX_TO_RK_SORTING");
    setConfigVariable(config.input->numFusionFiles, configData, "NUM_INPUT_FUSION_FILES");
    setConfigVariable(config.input->inputFile, configData, "INPUT_FILE");
    setConfigVariable(config.input->inputFileList, configData, "INPUT_FILE_LIST");
    setConfigVariable(config.input->inputFileClasses, configData, "INPUT_FILE_CLASSES");
    setConfigVariable(config.input->inputImagesPath, configData, "INPUT_IMAGES_PATH");

    // Parse multiple fusion files.
    for (int i = 1; i <= config.input->numFusionFiles; i++) {
      std::string key = "INPUT_FILES_FUSION_" + std::to_string(i);
      auto it = configData.find(key);
      if (it != configData.end()) {
        config.input->fusionFiles.push_back(std::get<std::string>(it->second));
      }
    }

    // Output parameters.
    config.output = std::make_unique<OutputConfig>();
    setConfigVariable(config.output->outputFile, configData, "OUTPUT_FILE");
    setConfigVariable(config.output->fileFormat, configData, "OUTPUT_FILE_FORMAT");
    setConfigVariable(config.output->matrixType, configData, "OUTPUT_MATRIX_TYPE");
    setConfigVariable(config.output->rkFormat, configData, "OUTPUT_RK_FORMAT");
    setConfigVariable(config.output->filePath, configData, "OUTPUT_FILE_PATH");
    setConfigVariable(config.output->htmlPerFile, configData, "OUTPUT_HTML_RK_PER_FILE");
    setConfigVariable(config.output->htmlSize, configData, "OUTPUT_HTML_RK_SIZE");
    setConfigVariable(config.output->htmlColors, configData, "OUTPUT_HTML_RK_COLORS");
    setConfigVariable(config.output->htmlBeforeAfter, configData, "OUTPUT_HTML_RK_BEFORE_AFTER");
    setConfigVariable(config.output->logFilePath, configData, "OUTPUT_LOG_FILE_PATH");

    // Evaluation parameters.
    config.evaluation = std::make_unique<EvalConfig>();
    setConfigVariable(config.evaluation->efficiencyEval, configData, "EFFICIENCY_EVAL");
    setConfigVariable(config.evaluation->effectivenessEval, configData, "EFFECTIVENESS_EVAL");
    setConfigVariable(config.evaluation->computePrecisions, configData, "EFFECTIVENESS_COMPUTE_PRECISIONS");
    setConfigVariable(config.evaluation->computeMap, configData, "EFFECTIVENESS_COMPUTE_MAP");
    setConfigVariable(config.evaluation->computeRecall, configData, "EFFECTIVENESS_COMPUTE_RECALL");

    // Parse multi-value parameters: recalls and precisions.
    std::string recallStr, precisionStr;
    setConfigVariable(recallStr, configData, "EFFECTIVENESS_RECALLS_TO_COMPUTE");
    setConfigVariable(precisionStr, configData, "EFFECTIVENESS_PRECISIONS_TO_COMPUTE");

    // Helper function to parse comma-separated values into a vector<int>
    auto parseCommaSeparatedInts = [](const std::string &str) -> std::vector<int> {
      std::vector<int> values;
      std::istringstream stream(str);
      std::string token;
      while (std::getline(stream, token, ',')) {  // Split by ','
        try {
          values.push_back(std::stoi(token));  // Convert to int
        } catch (const std::invalid_argument &e) {
          std::cerr << "Invalid integer found in config: " << token << std::endl;
          exit(1);  // Handle error appropriately
        }
      }
      return values;
    };

    // Parse values and store them in the config evaluation structure.
    config.evaluation->recallsToCompute = parseCommaSeparatedInts(recallStr);
    config.evaluation->precisionsToCompute = parseCommaSeparatedInts(precisionStr);

    // Method-specific parameters: those keys that contain "PARAM_" followed by the method name.
    config.methodParams = std::make_unique<MethodConfig>();
    for (const auto &pair : configData) {
      if (pair.first.find("PARAM_" + config.method) != std::string::npos) {
        config.methodParams->params[pair.first] = pair.second;
      }
    }
  }

  // Returns the stored Config object.
  const Config &getConfig() const { return config; }

 private:
  Config config;

  // Private constructor for singleton.
  ConfigProvider() = default;

  // Template helper function to set a configuration variable from the configData map.
  template <typename T>
  void setConfigVariable(T &dest, const std::map<std::string, ParamTypeVariants> &configData, const std::string &key) {
    auto it = configData.find(key);
    if (it != configData.end()) {
      try {
        dest = std::get<T>(it->second);
      } catch (const std::bad_variant_access &) {
        throw std::runtime_error("Type mismatch for variable: " + key);
      }
    }
  }
};

#endif  // CONFIG_PROVIDER_HPP
