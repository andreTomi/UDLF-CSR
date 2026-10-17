#include "Methods/hpp/LHRRSparseCore.hpp"
#include "../../ports/LHRR/k_b/lhrr_kernels.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<uint32_t> dense_initial_sort(const std::vector<uint32_t>& input,
                                         int n, int l) {
  std::vector<float> dense(static_cast<size_t>(n) * n, 0.0f);
  for (int rk = 0; rk < n; ++rk)
    for (int pos = 0; pos < l; ++pos) {
      const int img = input[static_cast<size_t>(l) * rk + pos];
      dense[static_cast<size_t>(n) * rk + img] += static_cast<float>(l - pos);
    }
  for (int rk = 0; rk < n; ++rk)
    for (int pos = 0; pos < l; ++pos) {
      const int img = input[static_cast<size_t>(l) * rk + pos];
      dense[static_cast<size_t>(n) * img + rk] += static_cast<float>(l - pos);
    }

  std::vector<uint32_t> result = input;
  std::vector<float> scores(l);
  std::vector<uint32_t> row(l);
  for (int rk = 0; rk < n; ++rk) {
    for (int j = 0; j < l; ++j) {
      row[j] = input[static_cast<size_t>(l) * rk + j];
      scores[j] = dense[static_cast<size_t>(n) * rk + row[j]];
    }
    lhrr_sparse::k2_sort(scores.data(), row.data(), l, rk);
    for (int j = 0; j < l; ++j)
      result[static_cast<size_t>(l) * rk + j] = row[j];
  }
  return result;
}

std::vector<uint32_t> literal_join(const std::vector<uint32_t>& list_work,
                                  const std::vector<int>& candidates,
                                  const std::vector<float>& a1,
                                  const std::vector<float>& a2) {
  std::vector<uint32_t> output = list_work;
  if (candidates.empty()) return output;
  size_t j = 0;
  const int l = static_cast<int>(list_work.size());
  for (int i = 1; i < l; ++i) {
    if (a2[j] > a1[i]) {
      for (int o = l - 1; o > i; --o) output[o] = output[o - 1];
      output[i] = static_cast<uint32_t>(candidates[j]);
      ++j;
      if (j >= candidates.size()) break;
    }
  }
  return output;
}

void test_initial_sort() {
  constexpr int n = 17;
  constexpr int l = 11;
  std::vector<uint32_t> input(static_cast<size_t>(n) * l);
  for (int rk = 0; rk < n; ++rk) {
    for (int pos = 0; pos < l; ++pos)
      input[static_cast<size_t>(l) * rk + pos] =
          static_cast<uint32_t>((rk * 7 + pos * 5 + pos / 2) % n);
    input[static_cast<size_t>(l) * rk + 0] = static_cast<uint32_t>(rk);
  }

  const auto expected = dense_initial_sort(input, n, l);
  auto production = input;
  auto production_w = lhrr_sparse::buildW(input.data(), n, l);
  lhrr_sparse::initial_sort_rankings(production_w, production.data(), n, l);
  require(production == expected, "production sparse initial_sort differs from dense K1/K2");

  auto k_b = input;
  auto k_b_w = lhrr::buildW(input.data(), n, l);
  lhrr::initial_sort_rankings(k_b_w, k_b.data(), n, l);
  require(k_b == expected, "K-b sparse initial_sort differs from dense K1/K2");

  std::vector<uint32_t> unique_input(static_cast<size_t>(n) * l);
  for (int rk = 0; rk < n; ++rk) {
    unique_input[static_cast<size_t>(l) * rk] = static_cast<uint32_t>(rk);
    int at = 1;
    for (int id = 0; id < n && at < l; ++id)
      if (id != rk)
        unique_input[static_cast<size_t>(l) * rk + at++] = static_cast<uint32_t>(id);
  }
  const auto unique_expected = dense_initial_sort(unique_input, n, l);
  auto unique_actual = unique_input;
  const auto unique_w = lhrr_sparse::buildW(unique_input.data(), n, l);
  lhrr_sparse::initial_sort_rankings(unique_w, unique_actual.data(), n, l);
  require(unique_actual == unique_expected,
          "production sparse initial_sort differs from dense on unique rows");

  bool production_rejected = false;
  try { (void)lhrr_sparse::buildW(nullptr, 1, 65536); }
  catch (const std::runtime_error&) { production_rejected = true; }
  require(production_rejected, "production W accepted L >= 65536");

  bool k_b_rejected = false;
  try { (void)lhrr::buildW(nullptr, 1, 65536); }
  catch (const std::invalid_argument&) { k_b_rejected = true; }
  require(k_b_rejected, "K-b W accepted L >= 65536");
}

void test_k9_differential() {
  std::mt19937 rng(0x4c485252u);
  std::uniform_int_distribution<int> list_length(2, 40);
  std::uniform_int_distribution<int> score(-4, 8);  // deliberate ties
  std::uniform_int_distribution<int> candidate_count(0, 240);  // many > L
  std::uniform_int_distribution<uint32_t> candidate_id(0, 1000000);

  for (int trial = 0; trial < 20000; ++trial) {
    const int l = list_length(rng);
    const int count = trial % 3 == 0
                          ? l + 1 + candidate_count(rng)
                          : candidate_count(rng);
    std::vector<uint32_t> list_work(l);
    std::vector<int> candidates(count);
    std::vector<float> a1(l);
    std::vector<float> a2(count);
    for (int i = 0; i < l; ++i) {
      list_work[i] = static_cast<uint32_t>(i);
      a1[i] = static_cast<float>(score(rng));
    }
    for (int i = 0; i < count; ++i) {
      candidates[i] = static_cast<int>(candidate_id(rng));
      a2[i] = static_cast<float>(score(rng));
    }
    // Guarantee the inclusive lower endpoint is covered as well as IDs far
    // above L; candidate identities need not be adjacent to the current row.
    if (count > 0) {
      if (trial % 10 == 0) candidates[0] = 0;
      candidates[count - 1] = 1000000;
    }
    const auto expected = literal_join(list_work, candidates, a1, a2);
    const auto actual = lhrr_sparse::k9_join_linear(list_work, candidates, a1, a2);
    require(actual == expected, "linear K9 differs from literal joinRks");
  }
}

}  // namespace

int main() {
  try {
    test_initial_sort();
    test_k9_differential();
    std::puts("LHRR_KERNELS,PASS,initial_sort_dense_differential=1,k9_trials=20000");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LHRR_KERNELS,FAIL,%s\n", error.what());
    return 1;
  }
}