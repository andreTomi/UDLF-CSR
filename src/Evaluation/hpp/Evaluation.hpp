#ifndef EVALUATION_HPP
#define EVALUATION_HPP

#include <omp.h>

#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "ConfigProvider.hpp"
#include "DataProvider.hpp"
#include "EventQueue.hpp"
#include "Logger.hpp"

class IEvaluationStrategy {
 public:
  virtual ~IEvaluationStrategy() = default;
  virtual float compute(int k) const = 0;

 protected:
  DataProvider& dataProvider = DataProvider::getInstance();
  int n = dataProvider.getN();
  VirtualArray<uint32_t>* rkLists = &dataProvider.getRkLists();
  const std::vector<uint32_t>& imageIds = dataProvider.getImageIds();
  const std::map<uint32_t, uint32_t>& classSizes = dataProvider.getClassIdSizes();
};

class PrecisionStrategy : public IEvaluationStrategy {
 public:
  float compute(int k) const override {
    const size_t imageIdsSize = imageIds.size();
    const size_t totalComparisons = static_cast<size_t>(n) * k;
    uint64_t totalCorrect = 0;

    for (int i = 0; i < n; i++) {
      const uint32_t queryClassId = imageIds[i];
      const size_t rowOffset = static_cast<size_t>(i) * n;

      for (int j = 0; j < k; j++) {
        const size_t matrixIndex = rowOffset + j;
        const uint32_t retrievedIndex = (*rkLists)[matrixIndex];

        if (retrievedIndex < imageIdsSize && imageIds[retrievedIndex] == queryClassId) {
          totalCorrect++;
        }
      }
    }

    return static_cast<float>(totalCorrect) / totalComparisons;
  }
};

class RecallStrategy : public IEvaluationStrategy {
 public:
  float compute(int k) const override {
    float recallSum = 0.0f;

    for (int i = 0; i < n; i++) {
      const uint32_t queryClassId = imageIds[i];
      auto it = classSizes.find(queryClassId);
      if (it == classSizes.end() || it->second == 0) continue;

      const uint32_t relevantTotal = it->second;
      const size_t rowOffset = static_cast<size_t>(i) * n;
      uint32_t relevantRetrieved = 0;

      for (int j = 0; j < k; j++) {
        const uint32_t retrievedIndex = (*rkLists)[rowOffset + j];
        if (retrievedIndex < imageIds.size() && imageIds[retrievedIndex] == queryClassId) {
          relevantRetrieved++;
        }
      }

      recallSum += static_cast<float>(relevantRetrieved) / relevantTotal;
    }

    return recallSum / n;
  }
};

class MAPStrategy : public IEvaluationStrategy {
 public:
  float compute(int k) const override {
    float mapSum = 0.0f;

    for (int i = 0; i < n; i++) {
      const uint32_t queryClassId = imageIds[i];
      auto it = classSizes.find(queryClassId);
      if (it == classSizes.end() || it->second == 0) continue;

      const uint32_t relevantTotal = it->second;
      const size_t rowOffset = static_cast<size_t>(i) * n;
      float sumPrecision = 0.0f;
      uint32_t relevantRetrieved = 0;

      for (int j = 0; j < k; j++) {
        const uint32_t retrievedIndex = (*rkLists)[rowOffset + j];
        if (retrievedIndex < imageIds.size() && imageIds[retrievedIndex] == queryClassId) {
          relevantRetrieved++;
          sumPrecision += static_cast<float>(relevantRetrieved) / (j + 1);
        }
      }

      mapSum += sumPrecision / std::min(static_cast<uint32_t>(k), relevantTotal);
    }

    return mapSum / n;
  }
};

class Evaluation {
 public:
  explicit Evaluation(const Config& config)
      : config(config),
        n(config.datasetSize),
        dataProvider(DataProvider::getInstance()),
        rkLists(&dataProvider.getRkLists()) {}

  void evaluate(const fs::path& inputFile, const fs::path& outputFile) {
    if (!config.evaluation->effectivenessEval) return;

    std::cout << "[Evaluation] Loading ranked lists for effectiveness evaluation...\n";

    rkLists->assignFromTextFile(inputFile, n, n);
    std::cout << "[Evaluation] Evaluating effectiveness before reranking...\n";
    evaluateEffectiveness();

    rkLists->assignFromTextFile(outputFile, n, n);
    std::cout << "[Evaluation] Evaluating effectiveness after reranking...\n";
    evaluateEffectiveness();
  }

  void evaluateEffectiveness() {
    std::map<int, float> precisions;
    std::map<int, float> recalls;
    float mapValue = 0.0f;

    std::mutex precisionsMutex;
    std::mutex recallsMutex;
    std::mutex mapMutex;

    auto& eventQueue = EventQueue::getInstance();

    if (config.evaluation->computePrecisions) {
      for (int k : config.evaluation->precisionsToCompute) {
        eventQueue.registerEvent(EventType::COMPUTE_PRECISION, [k, &precisions, &precisionsMutex]() {
          PrecisionStrategy precisionStrategy;
          float result = precisionStrategy.compute(k);
          std::lock_guard<std::mutex> lock(precisionsMutex);
          precisions[k] = result;
        });
      }
    }

    if (config.evaluation->computeRecall) {
      for (int k : config.evaluation->recallsToCompute) {
        eventQueue.registerEvent(EventType::COMPUTE_RECALL, [k, &recalls, &recallsMutex]() {
          RecallStrategy recallStrategy;
          float result = recallStrategy.compute(k);
          std::lock_guard<std::mutex> lock(recallsMutex);
          recalls[k] = result;
        });
      }
    }

    if (config.evaluation->computeMap) {
      int l = dataProvider.getL();
      eventQueue.registerEvent(EventType::COMPUTE_MAP, [&mapValue, &mapMutex, l, this]() {
        MAPStrategy mapStrategy;
        float result = mapStrategy.compute(l);
        std::lock_guard<std::mutex> lock(mapMutex);
        mapValue = result;
      });
    }

    eventQueue.processEvents();
    Logger::getInstance().logEffectiveness(precisions, recalls, mapValue);
  }

 private:
  const Config& config;
  int n;
  DataProvider& dataProvider;
  VirtualArray<uint32_t>* rkLists;
};

#endif  // EVALUATION_HPP
