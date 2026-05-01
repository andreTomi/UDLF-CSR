#ifndef METHOD_HPP
#define METHOD_HPP

#include <omp.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <vector>

#include "Config.hpp"
#include "ConfigProvider.hpp"
#include "DataProvider.hpp"
#include "Evaluation.hpp"
#include "EventQueue.hpp"
#include "FileIO.hpp"
#include "Logger.hpp"
#include "Time.hpp"
#include "Type.hpp"
#include "Validator.hpp"
#include "VirtualArray.hpp"

namespace fs = std::filesystem;

class Method {
 public:
  explicit Method()
      : config(ConfigProvider::getInstance().getConfig()),
        dataProvider(DataProvider::getInstance()),
        evaluation(config),
        rkLists(&dataProvider.getRkLists()),
        imgList(&dataProvider.getImageList()),
        n(config.get<int>("SIZE_DATASET")) {}

  void run() {
    try {
      std::cout << "Execution started at: " << Time::getCurrentTime() << std::endl;
      preprocess();
      EventQueue::getInstance().processEvents();
      Time::measureExecutionTime([this]() { execute(); });
      postprocess();
      finalize();
      std::cout << "Execution finished at: " << Time::getCurrentTime() << std::endl;
    } catch (const std::exception& e) {
      logMessage("Execution Error: " + std::string(e.what()));
    }
  }

 protected:
  const Config& config;
  DataProvider& dataProvider;
  Evaluation evaluation;

  VirtualArray<uint32_t>* rkLists;
  std::vector<std::string>* imgList;

  uint32_t n, l;

  virtual void preprocess() = 0;
  virtual void execute() = 0;
  virtual void postprocess() = 0;

  void logMessage(const std::string& msg) const {
    std::cout << msg << std::endl;
  }

 protected:
  template <typename T>
  void clearAndRelease(T& container) {
    T().swap(container);  // Libera memória via swap idiom
  }

 private:
  void finalize() {
    std::cout << "\n[Distance Matrix] Writing to output file: " << config.output->filePath << std::endl;

    std::cout << "\tGenerating distance matrix and writing ranked lists..." << std::endl;
    FileIO::generateDistanceMatrixAndWriteRankedLists(
        config.output->filePath, 
        "output/reranked_lists.txt", 
        n
    );

    evaluation.evaluate(config.input->inputFile, "output/reranked_lists.txt");
  }
};

#endif  // METHOD_HPP