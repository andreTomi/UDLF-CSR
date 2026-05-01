#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

#include "Config.hpp"
#include "ConfigProvider.hpp"
#include "Singleton.hpp"
#include "Time.hpp"

class Logger : public Singleton<Logger> {
 public:
  friend class Singleton<Logger>;
  // Get the singleton instance

  // Initialize Logger with Config object
  void initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    config = &ConfigProvider::getInstance().getConfig();  // Store a reference to config
    logFilePath = config->output->logFilePath;

    std::ofstream file(logFilePath);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open log file for writing: " + logFilePath);
    }
    file << std::fixed << std::setprecision(4);
    file.close();
  }

  // Log general information
  void logGeneralInfo(int datasetSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(logFilePath, std::ios::app);
    file << "\n\n- GENERAL INFORMATION -\n";
    file << "--------------------------------------";
    file << "\n Method:           " << config->method;
    file << "\n Dataset Size:     " << datasetSize;
    file << "\n Input File:       " << config->get<std::string>("INPUT_FILE");
    file << "\n Input Format:     " << config->get<std::string>("INPUT_FILE_FORMAT");
    file << "\n --------------------------------------\n";
    file.close();
  }

  // Log execution time
  void logExecutionTime(double executionTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(logFilePath, std::ios::app);
    file << "\n\n- EXECUTION TIME -\n";
    file << "--------------------------------------";
    file << "\n Total Execution Time: " << executionTime << " s\n";
    file << "--------------------------------------\n";
    file.close();
  }

  void logEffectiveness(const std::map<int, float> &precisions, const std::map<int, float> &recalls, float map) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(logFilePath, std::ios::app);
    file << "\n\nEFFECTIVENESS\n";
    writeMetrics(file, precisions, "P");
    writeMetrics(file, recalls, "Recall");
    file << "\n MAP\t" << (map * 100) << std::endl;
    file.close();
  }

  // Log relative gains
  void logRelativeGains(const std::map<int, float> &before, const std::map<int, float> &after, float mapBefore, float mapAfter) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(logFilePath, std::ios::app);
    file << "\n\n- RELATIVE GAINS -\n";
    file << "--------------------------------------";
    writeRelativeGains(file, before, after, "P");
    writeRelativeGains(file, before, after, "Recall");
    file << "\n MAP\t" << (((mapAfter - mapBefore) / mapBefore) * 100) << "%";
    file << "\n--------------------------------------\n";
    file.close();
  }

  // Finalize log file
  void finalizeLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(logFilePath, std::ios::app);
    file << "\n Log generated at " << Time::getCurrentTime() << std::endl;
    file.close();
  }

 private:
  Logger() = default;  // Private constructor for singleton

  std::mutex mutex_;
  std::string logFilePath;
  const Config *config;  // Pointer to Config object

  // Helper function to write metric maps
  void writeMetrics(std::ofstream &file, const std::map<int, float> &metrics, const std::string &metricName) {
    for (const auto &m : metrics) {
      file << "\n " << metricName << "@" << m.first << "\t" << (m.second * 100) << "%";
    }
  }

  void writeRelativeGains(std::ofstream &file, const std::map<int, float> &before, const std::map<int, float> &after, const std::string &metricName) {
    for (const auto &m : after) {
      float prev = before.at(m.first);  // Ensures safe access
      file << "\n " << metricName << "@" << m.first << "\t" << (((m.second - prev) / prev) * 100) << "%";
    }
  }
};

#endif  // LOGGER_HPP
