#ifndef FILE_IO_HPP
#define FILE_IO_HPP

#include <omp.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ConfigProvider.hpp"
#include "DataProvider.hpp"
#include "Type.hpp"
#include "VirtualArray.hpp"

class FileIO {
 public:
  // Reads all lines from a file into a vector of strings
  static std::vector<std::string> readFileToLines(const std::string &filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open file: " + filename);
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
      // Remove spaces and comments
      line.erase(std::remove(line.begin(), line.end(), ' '), line.end());
      line.erase(std::remove(line.begin(), line.end(), '\t'), line.end());
      line = line.substr(0, line.find("#", 0));
      size_t pos = line.find_last_not_of("\r");
      line = line.substr(0, pos + 1);
      if (!line.empty()) {
        lines.push_back(line);
      }
    }
    return lines;
  }

  // Fused function: generates distance matrix and writes ranked lists in one pass
  static void generateDistanceMatrixAndWriteRankedLists(
      const std::string &matrixFilepath,
      const std::string &rankedListsFilepath, 
      int n) {
    
    DataProvider &dp = DataProvider::getInstance();
    VirtualArray<uint32_t> &rkLists = dp.getRkLists();
    int l = rkLists.size() / n;
    std::string inputFilepath = ConfigProvider::getInstance().getConfig().input->inputFile;

    // Read input file for ranked lists
    std::ifstream inputFile(inputFilepath);
    if (!inputFile) {
      throw std::runtime_error("Failed to open input file: " + inputFilepath);
    }

    std::vector<std::string> inputLines(n);
    for (uint64_t i = 0; i < n; ++i) {
      if (!std::getline(inputFile, inputLines[i])) {
        throw std::runtime_error("Failed to read line " + std::to_string(i) + " from input file");
      }
    }
    inputFile.close();

    // Open output files
    std::ofstream matrixFile(matrixFilepath, std::ios::binary);
    std::ofstream rankedListsFile(rankedListsFilepath, std::ios::binary);
    
    if (!matrixFile) {
      throw std::runtime_error("Failed to open matrix output file: " + matrixFilepath);
    }
    if (!rankedListsFile) {
      throw std::runtime_error("Failed to open ranked lists output file: " + rankedListsFilepath);
    }

    constexpr size_t BUFFER_SIZE = 65536;
    std::string matrixBuffer, rankedListsBuffer;
    matrixBuffer.reserve(BUFFER_SIZE);
    rankedListsBuffer.reserve(BUFFER_SIZE);

    // Process each row
    for (size_t row = 0; row < n; ++row) {
      const size_t row_offset = row * l;

      // Temporary storage for this row's distance values
      std::vector<uint32_t> rowDistances(n, 0);

      // Compute distance matrix row from ranked list
      for (uint32_t rank = 0; rank < l; ++rank) {
        const uint32_t img = rkLists[row_offset + rank];
        const uint32_t current = rowDistances[img];
        const uint32_t sum = current + rank;
        rowDistances[img] = (sum >= l) ? (sum - l) : 0;
      }

      // Write distance matrix row to buffer
      std::string matrixRow;
      matrixRow.reserve(n * 12);
      for (size_t col = 0; col < n; ++col) {
        matrixRow += std::to_string(rowDistances[col]);
        if (col < n - 1) {
          matrixRow += ' ';
        }
      }
      matrixRow += '\n';

      if (matrixBuffer.size() + matrixRow.size() > BUFFER_SIZE) {
        matrixFile.write(matrixBuffer.data(), matrixBuffer.size());
        matrixBuffer.clear();
      }
      matrixBuffer += matrixRow;

      // Generate ranked lists output line
      std::istringstream iss(inputLines[row]);
      std::string rankedListLine;
      rankedListLine.reserve(n * 12);

      std::vector<uint32_t> inputValues;
      uint32_t val;
      while (iss >> val) {
        inputValues.push_back(val);
      }

      // Replace first l values with reranked list
      for (uint64_t j = 0; j < l && j < inputValues.size(); ++j) {
        rankedListLine += std::to_string(rkLists[row_offset + j]) + ' ';
      }

      // Add remaining values from original input
      for (uint64_t k = l; k < inputValues.size(); ++k) {
        rankedListLine += std::to_string(inputValues[k]) + ' ';
      }

      // Remove last space and add newline
      if (!rankedListLine.empty() && rankedListLine.back() == ' ') {
        rankedListLine.back() = '\n';
      } else {
        rankedListLine += '\n';
      }

      if (rankedListsBuffer.size() + rankedListLine.size() > BUFFER_SIZE) {
        rankedListsFile.write(rankedListsBuffer.data(), rankedListsBuffer.size());
        rankedListsBuffer.clear();
      }
      rankedListsBuffer += rankedListLine;
    }

    // Write remaining buffers
    if (!matrixBuffer.empty()) {
      matrixFile.write(matrixBuffer.data(), matrixBuffer.size());
    }
    if (!rankedListsBuffer.empty()) {
      rankedListsFile.write(rankedListsBuffer.data(), rankedListsBuffer.size());
    }

    matrixFile.close();
    rankedListsFile.close();
  }

  static void writeRankedListsToFile(const std::string &filepath, int n) {
    DataProvider &dp = DataProvider::getInstance();
    VirtualArray<uint32_t> &rkLists = dp.getRkLists();
    int l = rkLists.size() / n;
    std::string inputFilepath = ConfigProvider::getInstance().getConfig().input->inputFile;

    std::ifstream inputFile(inputFilepath);
    if (!inputFile) {
      throw std::runtime_error("Failed to open input file: " + inputFilepath);
    }

    std::vector<std::string> inputLines(n);
    for (uint64_t i = 0; i < n; ++i) {
      if (!std::getline(inputFile, inputLines[i])) {
        throw std::runtime_error("Failed to read line " + std::to_string(i) + " from input file");
      }
    }
    inputFile.close();

    std::vector<std::string> outputLines(n);

#pragma omp parallel for
    for (uint64_t i = 0; i < n; ++i) {
      std::istringstream iss(inputLines[i]);
      std::string line;
      line.reserve(n * 12);

      std::vector<uint32_t> inputValues;
      uint32_t val;
      while (iss >> val) {
        inputValues.push_back(val);
      }

      for (uint64_t j = 0; j < l && j < inputValues.size(); ++j) {
        line += std::to_string(rkLists[i * l + j]) + ' ';
      }

      for (uint64_t k = l; k < inputValues.size(); ++k) {
        line += std::to_string(inputValues[k]) + ' ';
      }

      if (!line.empty() && line.back() == ' ') {
        line.back() = '\n';
      } else {
        line += '\n';
      }

      outputLines[i] = std::move(line);
    }

    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Failed to open output file: " + filepath);
    }

    constexpr size_t BUFFER_SIZE = 65536;
    std::string buffer;
    buffer.reserve(BUFFER_SIZE);

    for (const auto &outputLine : outputLines) {
      if (buffer.size() + outputLine.size() > BUFFER_SIZE) {
        file.write(buffer.data(), buffer.size());
        buffer.clear();
      }
      buffer += outputLine;
    }

    if (!buffer.empty()) {
      file.write(buffer.data(), buffer.size());
    }
  }
};

#endif  // FILE_IO_HPP