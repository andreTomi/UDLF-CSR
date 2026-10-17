// Standalone core gate used under ASan/UBSan and production fast-math flags.
#include "Methods/hpp/CprrSparseCore.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint64_t read_le(const std::vector<uint8_t>& bytes, size_t& pos, size_t width) {
  if (pos + width > bytes.size()) throw std::runtime_error("short SNP1 ranking");
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes[pos++]) << (8 * i);
  return value;
}

uint64_t fnv1a64(const uint8_t* bytes, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<uint32_t> read_lists(const std::string& path, size_t n, size_t L) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open fixture input " + path);
  std::vector<uint32_t> values;
  values.reserve(n * L);
  uint64_t value;
  while (in >> value) {
    if (value > UINT32_MAX) throw std::runtime_error("fixture ID exceeds uint32_t");
    values.push_back(static_cast<uint32_t>(value));
  }
  if (!in.eof() || values.size() != n * L)
    throw std::runtime_error("fixture input token count differs from n*L");
  return values;
}

std::vector<uint32_t> read_expected(const std::string& path, size_t n, size_t L) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open expected snapshot " + path);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
  if (bytes.size() < 44 || bytes[0] != 'S' || bytes[1] != 'N' ||
      bytes[2] != 'P' || bytes[3] != '1')
    throw std::runtime_error("invalid expected SNP1 ranking");
  size_t pos = 4;
  if (read_le(bytes, pos, 4) != 1) throw std::runtime_error("unsupported SNP1 version");
  if (read_le(bytes, pos, 1) != 3 || read_le(bytes, pos, 1) != 3)
    throw std::runtime_error("expected snapshot is not int32 ranking layout");
  pos += 2;
  const uint64_t rows = read_le(bytes, pos, 8);
  const uint64_t cols = read_le(bytes, pos, 8);
  const uint64_t count = read_le(bytes, pos, 8);
  if (rows != n || cols != L || count != n * L || bytes.size() != 36 + count * 4 + 8)
    throw std::runtime_error("expected snapshot dimensions/length mismatch");
  size_t trailer = bytes.size() - 8;
  const uint64_t stored_hash = read_le(bytes, trailer, 8);
  if (fnv1a64(bytes.data(), bytes.size() - 8) != stored_hash)
    throw std::runtime_error("expected snapshot content hash mismatch");
  std::vector<uint32_t> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i)
    values.push_back(static_cast<uint32_t>(static_cast<int32_t>(read_le(bytes, pos, 4))));
  return values;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 9) {
      std::fprintf(stderr, "usage: test_cprr_core N L K T INPUT EXPECTED.snp FIXTURE OUTPUT.txt\n");
      return 2;
    }
    const size_t n = std::stoull(argv[1]);
    const size_t L = std::stoull(argv[2]);
    const size_t k = std::stoull(argv[3]);
    const size_t t = std::stoull(argv[4]);
    const auto input = read_lists(argv[5], n, L);
    const auto expected = read_expected(argv[6], n, L);
    cprr::validate_input_I4(input.data(), n, L, k, "CPRR_CORE_GATE");
    auto state = cprr::make_state(input, n, L, k, t, L);
    cprr::k1_fillPos(state);
    cprr::k2_sortZero(state);
    for (size_t iteration = 1; iteration <= t; ++iteration) {
      cprr::k3_cartProd(state);
      cprr::k4_reverseCartProd(state);
      cprr::k5_cleanRef(state);
      if (!state.rev_src.empty() || !state.rev_pos.empty() ||
          state.rev_rowptr.size() != n + 1)
        throw std::runtime_error("K5 did not clear reverse CSR");
      for (size_t refs : state.nRef)
        if (refs != 0) throw std::runtime_error("K5 did not clear nRef");
      if (iteration == t) cprr::k6_sort(state);
      else cprr::k2_sortZero(state);
    }
    if (state.lists != expected) {
      size_t i = 0;
      while (i < expected.size() && state.lists[i] == expected[i]) ++i;
      throw std::runtime_error("ranking differs at row " + std::to_string(i / L) +
                               ", position " + std::to_string(i % L));
    }
    std::ofstream output(argv[8]);
    if (!output) throw std::runtime_error("cannot create final ranking output");
    for (size_t r = 0; r < n; ++r) {
      for (size_t j = 0; j < L; ++j) {
        if (j) output << ' ';
        output << state.lists[r * L + j];
      }
      output << '\n';
    }
    if (!output) throw std::runtime_error("failed writing final ranking output");
    std::printf("CPRR_CORE,PASS,fixture=%s,n=%zu,L=%zu,k=%zu,t=%zu,ids=%zu\n",
                argv[7], n, L, k, t, state.lists.size());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "CPRR_CORE,FAIL,%s\n", e.what());
    return 1;
  }
}