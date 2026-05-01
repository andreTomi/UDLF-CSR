#ifndef DATA_LOADER_HPP
#define DATA_LOADER_HPP

#include <omp.h>

#include <bitset>
#include <filesystem>
#include <iostream>
#include <map>
#include <unordered_set>
#include <vector>

#include "ConfigProvider.hpp"
#include "DataProvider.hpp"
#include "EventQueue.hpp"
#include "FileIO.hpp"
#include "Singleton.hpp"

namespace fs = std::filesystem;

class DataLoader : public Singleton<DataLoader> {
 public:
  friend class Singleton<DataLoader>;

  void initialize() {
    this->config = &ConfigProvider::getInstance().getConfig();

    // Register event listeners
    EventQueue::getInstance().registerEvent(EventType::LOAD_CLASSES_FILE, [this]() { loadClassesFile(); });
    EventQueue::getInstance().registerEvent(EventType::LOAD_IMAGE_LIST, [this]() { loadImageList(); });
    EventQueue::getInstance().registerEvent(EventType::LOAD_RANKED_LISTS, [this]() { loadRankedLists(); });
  }

 private:
  DataLoader() : dataProvider(DataProvider::getInstance()),
                 rkLists(dataProvider.getRkLists()),
                 imageList(dataProvider.getImageList()) {}

  const Config* config;

  DataProvider& dataProvider;

  // Direct references to DataProvider's data
  VirtualArray<uint32_t>& rkLists;

  std::vector<std::string>& imageList;

  void loadRankedLists() {
    auto& cfg = *config;
    const auto& inputFile = cfg.input->inputFile;
    uint32_t n = cfg.datasetSize;
    uint32_t l = dataProvider.getL();

    std::cout << "[DataLoader]: Initializing Ranked Lists...\n";
    rkLists.assignFromTextFile(inputFile, n, l);

    std::cout << "[DataLoader]: Finished preprocessing ranked lists.\n";
  }

  void loadClassesFile() {
    std::cout << "[DataLoader]: Reading classes from file\n";

    std::vector<std::string> lines = FileIO::readFileToLines(config->input->inputFileClasses);

    std::vector<std::string> classNames;
    classNames.reserve(lines.size());

    for (const std::string& line : lines) {
      size_t pos = line.find(':');
      if (pos == std::string::npos) {
        throw std::runtime_error("Invalid format in classes file: " + line);
      }

      std::string className = line.substr(pos + 1);
      classNames.push_back(className);
    }

    computeImageIdsAndClassSizes(classNames);

    std::cout << "[DataLoader]: Loaded " << classNames.size()
              << " images in " << dataProvider.getClassIdSizes().size() << " classes." << std::endl;
  }

  void computeImageIdsAndClassSizes(const std::vector<std::string>& classNames) {
    std::map<std::string, uint32_t> localClassIds;
    std::map<uint32_t, uint32_t> classIdSizes;
    std::vector<uint32_t> imageIds;
    imageIds.reserve(classNames.size());

    uint32_t nextId = 0;
    for (const std::string& name : classNames) {
      if (localClassIds.find(name) == localClassIds.end()) {
        localClassIds[name] = nextId++;
      }
      uint32_t classId = localClassIds[name];
      imageIds.push_back(classId);
      classIdSizes[classId]++;
    }

    dataProvider.setImageIds(imageIds);
    dataProvider.setClassIdSizes(classIdSizes);
  }   

  void loadImageList() {
    std::cout << "[DataLoader]: Reading image list from file\n";
    imageList = FileIO::readFileToLines(config->input->inputFileList);
  }
};

#endif  // DATA_LOADER_HPP