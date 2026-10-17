// Standalone LHRR sparse core gate. Compares the faithful production core with
// an approved K-a CP8 ranking snapshot and checks repeatability in-process.
#include "Methods/hpp/LHRRSparseCore.hpp"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint32_t read_le32(std::istream& in) {
  unsigned char bytes[4];
  if (!in.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
    throw std::runtime_error("short snapshot header or payload");
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_le64(std::istream& in) {
  unsigned char bytes[8];
  if (!in.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
    throw std::runtime_error("short snapshot header");
  uint64_t value = 0;
  for (unsigned int i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  return value;
}

std::vector<uint32_t> read_ranking(const std::string& path, size_t n, size_t l) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open expected snapshot: " + path);
  char magic[4];
  if (!in.read(magic, sizeof(magic)) || std::string(magic, 4) != "SIMP")
    throw std::runtime_error("invalid SIMP snapshot: " + path);
  if (read_le32(in) != 1) throw std::runtime_error("unsupported SIMP version");
  (void)read_le32(in);  // checkpoint
  (void)read_le32(in);  // iteration
  (void)read_le32(in);  // chunk
  (void)read_le32(in);  // total chunks
  if (read_le64(in) != n || read_le64(in) != l || read_le64(in) == 0)
    throw std::runtime_error("snapshot dimensions do not match the test");
  const uint64_t count = read_le64(in);
  const uint32_t dtype = read_le32(in) & 0xffu;
  if (count != n * l || dtype != 1)
    throw std::runtime_error("snapshot is not a uint32 ranking of n*L elements");

  std::vector<uint32_t> values(count);
  for (uint32_t& value : values) value = read_le32(in);
  return values;
}

std::vector<uint32_t> read_input(const std::string& path, size_t n, size_t l) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open ranking input: " + path);
  std::vector<uint32_t> values;
  values.reserve(n * l);
  uint64_t value = 0;
  while (in >> value) {
    if (value > UINT32_MAX) throw std::runtime_error("input id exceeds uint32_t");
    values.push_back(static_cast<uint32_t>(value));
  }
  if (!in.eof() || values.size() != n * l)
    throw std::runtime_error("input does not contain exactly n*L ids");
  return values;
}

void compare(const std::vector<uint32_t>& actual,
             const std::vector<uint32_t>& expected, size_t l) {
  if (actual.size() != expected.size()) throw std::runtime_error("ranking size mismatch");
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected[i])
      throw std::runtime_error("ranking mismatch at row " + std::to_string(i / l) +
                               ", position " + std::to_string(i % l));
  }
}

std::vector<uint32_t> run_core(const std::vector<uint32_t>& input, int n, int l,
                               int k, int t, int threads) {
  lhrr_sparse::Pipeline pipeline;
  pipeline.init(n, l, k, t);
  pipeline.set_rkLists(input.data());
  pipeline.run(threads);
  return pipeline.get_result();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 7) {
      std::fprintf(stderr, "usage: test_lhrr_core N L K T INPUT EXPECTED.simp\n");
      return 2;
    }
    const size_t n = std::stoull(argv[1]);
    const size_t l = std::stoull(argv[2]);
    const int k = std::stoi(argv[3]);
    const int t = std::stoi(argv[4]);
    {
      lhrr_sparse::Pipeline invalid_threads;
      invalid_threads.init(static_cast<int>(n), static_cast<int>(l), k, t);
      bool rejected = false;
      try { invalid_threads.run(0); }
      catch (const std::runtime_error&) { rejected = true; }
      if (!rejected) throw std::runtime_error("zero thread count was accepted");
#ifndef _OPENMP
      rejected = false;
      try { invalid_threads.run(2); }
      catch (const std::runtime_error&) { rejected = true; }
      if (!rejected)
        throw std::runtime_error("parallel request without OpenMP was accepted");
#endif
    }
    const auto input = read_input(argv[5], n, l);
    const auto expected = read_ranking(argv[6], n, l);
    const auto serial = run_core(input, static_cast<int>(n), static_cast<int>(l), k, t, 1);
    compare(serial, expected, l);
    for (const int threads : {1, 2, 4, 8}) {
      const auto first = run_core(input, static_cast<int>(n), static_cast<int>(l),
                                  k, t, threads);
      const auto second = run_core(input, static_cast<int>(n), static_cast<int>(l),
                                   k, t, threads);
      compare(first, serial, l);
      compare(first, second, l);
      std::printf("LHRR_CORE_PARITY,PASS,n=%zu,L=%zu,k=%d,t=%d,threads=%d,ids=%zu\n",
                  n, l, k, t, threads, first.size());
    }
    if (n == 100 && l == 20 && k == 5) {
      auto adversarial = input;
      for (size_t row = 0; row < n; ++row) {
        uint32_t& first = adversarial[row * l];
        uint32_t& other = adversarial[row * l + 1];
        if (first == row) std::swap(first, other);
      }
      const auto adversarial_serial = run_core(adversarial, 100, 20, 5, t, 1);
      for (const int threads : {1, 2, 4, 8}) {
        const auto actual = run_core(adversarial, 100, 20, 5, t, threads);
        compare(actual, adversarial_serial, l);
        std::printf("LHRR_CORE_ADVERSARIAL,PASS,n=100,L=20,k=5,t=%d,threads=%d\n",
                    t, threads);
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LHRR_CORE,FAIL,%s\n", error.what());
    return 1;
  }
}