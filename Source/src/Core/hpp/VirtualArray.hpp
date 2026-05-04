#ifndef VIRTUAL_ARRAY_HPP
#define VIRTUAL_ARRAY_HPP

// Configuration: Choose one of these implementations via -D compiler flag.
// Default is VIRTUAL_ARRAY_PURE_MMAP if none is specified.
#if !defined(VIRTUAL_ARRAY_PURE_MMAP) && !defined(VIRTUAL_ARRAY_FULL_MEMORY)
    #define VIRTUAL_ARRAY_PURE_MMAP
#endif

#include <omp.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#if defined(VIRTUAL_ARRAY_PURE_MMAP)
#include <fcntl.h>
#include <sys/mman.h>
#endif

namespace fs = std::filesystem;

template <typename T>
class VirtualArray {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for memory mapping");

 public:
#if defined(VIRTUAL_ARRAY_PURE_MMAP)
  static std::unique_ptr<VirtualArray<T>> create(size_t rows, size_t cols, const std::string& filename) {
    fs::path temp_dir = "/tmp/udlf_temp";

    // Create temp directory if it doesn't exist
    if (!fs::exists(temp_dir)) {
      fs::create_directories(temp_dir);
    }

    const char* backing_file = ((temp_dir / filename).string()).c_str();
    return std::unique_ptr<VirtualArray<T>>(new VirtualArray<T>(rows, cols, backing_file));
  }
#else
  static std::unique_ptr<VirtualArray<T>> create(size_t rows, size_t cols, std::string backing_file = "") {
    return std::unique_ptr<VirtualArray<T>>(new VirtualArray<T>(rows, cols));
  }
#endif

  void assign(T value, size_t n, size_t l) {
    if (n * l > _rows * _cols) {
      throw std::out_of_range("Requested size exceeds allocated size");
    }

    size_t total = n * l;
    _size = total;  // Update the actual size

    std::cout << "\tAssigning " << total << " elements with value: " << value << std::endl;

    if (value == T{}) return;

#pragma omp parallel for simd
    for (size_t i = 0; i < total; ++i) {
      (*this)[i] = value;
    }
  }

  void assignFromTextFile(const std::string& infile, size_t n, size_t l) {
    std::ifstream file(infile);
    if (!file) throw std::runtime_error("Failed to open input file");

    std::vector<std::string> lines(n);
    for (size_t i = 0; i < n; ++i) {
      if (!std::getline(file, lines[i]))
        throw std::runtime_error("Not enough lines in file");
    }

    std::cout << "\tAssigning from text file: " << infile << " with " << n
              << " rows and " << l << " max columns\n";

    // Vector to store the number of columns read per row
    std::vector<size_t> cols_per_row(n, 0);

#pragma omp parallel for
    for (size_t i = 0; i < n; ++i) {
      cols_per_row[i] = processLine(lines[i], i, l);
    }

    // Calculate total size based on actual columns read
    size_t total_elements = 0;
    for (size_t i = 0; i < n; ++i) {
      total_elements += cols_per_row[i];
    }

    _size = total_elements;
  }

  T& operator[](size_t idx) {
    if (idx >= _rows * _cols) {
      throw std::out_of_range("Index out of bounds " + std::to_string(idx) + " >= " +
                              std::to_string(_rows * _cols));
    }
#ifdef VIRTUAL_ARRAY_PURE_MMAP
    return *reinterpret_cast<T*>(static_cast<char*>(_base) + idx * sizeof(T));
#else  // VIRTUAL_ARRAY_FULL_MEMORY
    return _data[idx];
#endif
  }

  const T& operator[](size_t idx) const {
    if (idx >= _rows * _cols) {
      throw std::out_of_range("Index out of bounds");
    }
#ifdef VIRTUAL_ARRAY_PURE_MMAP
    return *reinterpret_cast<const T*>(static_cast<const char*>(_base) + idx * sizeof(T));
#else  // VIRTUAL_ARRAY_FULL_MEMORY
    return _data[idx];
#endif
  }

#ifdef VIRTUAL_ARRAY_FULL_MEMORY
  // Touch memory pages in chunks to trigger swap allocation if needed
  void preallocate() {
    const size_t page_size = getpagesize();
    const size_t elements_per_page = page_size / sizeof(T);

#pragma omp parallel for
    for (size_t i = 0; i < _rows * _cols; i += elements_per_page) {
      // Touch one element per page to ensure it's allocated
      volatile T dummy = _data[i];
      (void)dummy;  // Suppress unused variable warning
    }
  }

  size_t rows() const { return _rows; }
  size_t cols() const { return _cols; }
  size_t totalBytes() const { return _totalBytes; }
#endif
  size_t size() const { return _size; }

  ~VirtualArray() {
#if defined(VIRTUAL_ARRAY_PURE_MMAP)
    if (_base != nullptr && _base != MAP_FAILED) {
      ::munmap(_base, _totalBytes);
    }
    if (fd >= 0) {
      ::close(fd);
      if (!_backing_file.empty()) {
        ::unlink(_backing_file.c_str());
      }
    }
#endif
#ifdef VIRTUAL_ARRAY_FULL_MEMORY
    if (_data != nullptr) {
      std::free(_data);
    }
#endif
  }

 private:
#if defined(VIRTUAL_ARRAY_PURE_MMAP)
  VirtualArray(size_t rows, size_t cols, const char* filename)
      : _base(nullptr),
        _rows(rows),
        _cols(cols),
        _totalBytes(rows * cols * sizeof(T)),
        _size(0),
        _backing_file(filename),
        fd(-1) {
    // Open and setup the backing file
    fd = ::open(_backing_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
      throw std::runtime_error("Failed to open backing file");
    }

    if (::ftruncate(fd, _totalBytes) < 0) {
      ::close(fd);
      fd = -1;
      throw std::runtime_error("Failed to set file size");
    }

    // Memory-map the entire file (including cached portion for initialization)
    _base = ::mmap(nullptr, _totalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (_base == MAP_FAILED) {
      ::close(fd);
      fd = -1;
      _base = nullptr;
      throw std::runtime_error("Failed to memory-map backing file");
    }

    // These are just hints, failures are OK
    ::madvise(_base, _totalBytes, MADV_HUGEPAGE);
    ::madvise(_base, _totalBytes, MADV_SEQUENTIAL);
  }
#else  // VIRTUAL_ARRAY_FULL_MEMORY
  VirtualArray(size_t rows, size_t cols)
      : _rows(rows),
        _cols(cols),
        _totalBytes(rows * cols * sizeof(T)),
        _data(nullptr),
        _size(0) {
    // Check if we have enough virtual memory space (on 64-bit systems this is usually fine)
    if (_totalBytes == 0 || _totalBytes / sizeof(T) != rows * cols) {
      throw std::overflow_error("Array size calculation overflow");
    }

    // Allocate memory - this will use swap if physical memory is insufficient
    _data = static_cast<T*>(std::malloc(_totalBytes));
    if (_data == nullptr) {
      throw std::bad_alloc();
    }

    // Optional: Initialize to zero (this will actually allocate the pages)
    // Comment out if you don't want to immediately allocate all pages
    // std::memset(_data, 0, _totalBytes);

    std::cout << "Allocated " << _totalBytes / (1024 * 1024)
              << " MB of virtual memory for array of type " << typeid(T).name() << std::endl;
  }
#endif

  size_t processLine(const std::string& line, size_t row, size_t cols) {
    std::istringstream iss(line);
    size_t elements_read = 0;

    for (size_t col = 0; col < cols; ++col) {
      T value;
      if (!(iss >> value)) {
        break;  // Stop if we can't read more values from this line
      }
      (*this)[row * cols + col] = value;
      elements_read++;
    }

    return elements_read;
  }

  // Member variables
#if defined(VIRTUAL_ARRAY_PURE_MMAP)
  void* _base;                // Pointer to memory-mapped data
  int fd;                     // File descriptor for the backing file
  std::string _backing_file;  // Path to the backing file (for cleanup)
#else                         // VIRTUAL_ARRAY_FULL_MEMORY
  T* _data;  // Pointer to allocated memory
#endif
  size_t _rows, _cols, _totalBytes, _size;
};

#endif  // VIRTUAL_ARRAY_HPP
