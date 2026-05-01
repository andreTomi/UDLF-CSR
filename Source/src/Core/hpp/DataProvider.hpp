#ifndef DATA_PROVIDER_HPP
#define DATA_PROVIDER_HPP

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.hpp"
#include "ConfigProvider.hpp"
#include "Singleton.hpp"
#include "VirtualArray.hpp"

namespace fs = std::filesystem;

class DataProvider : public Singleton<DataProvider> {
 public:
  friend class Singleton<DataProvider>;

  // Getters para arrays e listas originais
  VirtualArray<uint32_t>& getRkLists() { return *rkLists; }
  std::vector<std::string>& getImageList() { return imageList; }

  // Getters para dados pré-calculados (acesso rápido por referência)
  const std::vector<uint32_t>& getImageIds() const { return imageIds; }
  const std::map<uint32_t, uint32_t>& getClassIdSizes() const { return classIdSizes; }

  // Setters para dados pré-calculados
  void setImageIds(const std::vector<uint32_t>& ids) { imageIds = ids; }
  void setClassIdSizes(const std::map<uint32_t, uint32_t>& sizes) { classIdSizes = sizes; }

  uint32_t& getN() { return n; }
  uint32_t& getL() { return l; }
  void setL(uint32_t l) { this->l = l; }

 private:
  // CRITICAL FIX: Declare n first so it's initialized before use
  uint32_t n{static_cast<uint32_t>(ConfigProvider::getInstance().getConfig().datasetSize)};
  uint32_t l{0};  // Initialize l as well

  DataProvider() : rkLists(VirtualArray<uint32_t>::create(n, n, "rkLists.txt")) {}

  // Explicit destructor to ensure clean shutdown
  ~DataProvider() {
    // Reset unique_ptrs first to ensure proper cleanup order
    rkLists.reset();
  }

  // Dados pré-calculados armazenados como membros para acesso rápido
  std::vector<uint32_t> imageIds;
  std::map<uint32_t, uint32_t> classIdSizes;

  // Dados originais
  std::vector<std::string> imageList;

  std::unique_ptr<VirtualArray<uint32_t>> rkLists;
};

#endif  // DATA_PROVIDER_HPP