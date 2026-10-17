#ifndef CPRR_SPARSE_CORE_HPP
#define CPRR_SPARSE_CORE_HPP

// Serial sparse CPRR kernels K1-K6, copied from the validated isolated port.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cprr {

struct SparseState {
  size_t n = 0, L = 0, k = 0, t = 0, lval = 0;
  std::vector<uint32_t> lists, col_idx;
  std::vector<float> values;
  std::vector<size_t> rev_src, rev_pos, rev_rowptr, nRef;
};

inline void acc_float(float& cell, double score) {
  cell = static_cast<float>(static_cast<double>(cell) + score);
}

inline int64_t find_slot(const SparseState& st, size_t r, uint32_t c) {
  const size_t base = r * st.L;
  const auto first = st.col_idx.begin() + static_cast<std::ptrdiff_t>(base);
  const auto last = first + static_cast<std::ptrdiff_t>(st.L);
  const auto found = std::lower_bound(first, last, c);
  if (found == last || *found != c) return -1;
  return static_cast<int64_t>(found - first);
}

inline void validate_input_I4(const uint32_t* lists, size_t n, size_t L,
                              size_t k, const char* ctx) {
  if (n == 0 || L == 0) throw std::runtime_error(std::string(ctx) + ": n,L must be > 0");
  if (L > n) throw std::runtime_error(std::string(ctx) + ": L > n violates I4");
  if (k == 0) throw std::runtime_error(std::string(ctx) + ": k must be > 0 (I4)");
  if (k > L) throw std::runtime_error(std::string(ctx) + ": k > L violates I4");
  for (size_t r = 0; r < n; ++r) {
    bool has_self = false;
    std::vector<uint32_t> row;
    row.reserve(L);
    for (size_t j = 0; j < L; ++j) {
      const uint32_t c = lists[r * L + j];
      if (c >= n) throw std::runtime_error(std::string(ctx) + ": id out of range at row " + std::to_string(r));
      if (c == r) has_self = true;
      row.push_back(c);
    }
    std::sort(row.begin(), row.end());
    for (size_t j = 1; j < row.size(); ++j)
      if (row[j - 1] == row[j])
        throw std::runtime_error(std::string(ctx) + ": duplicate id in row " + std::to_string(r));
    if (!has_self)
      throw std::runtime_error(std::string(ctx) + ": self missing in row " + std::to_string(r) + " (I4)");
  }
}

inline SparseState make_state(std::vector<uint32_t> input_lists,
                              size_t n, size_t L, size_t k, size_t t,
                              size_t lval) {
  if (n != 0 && L > input_lists.max_size() / n)
    throw std::runtime_error("state: n*L overflows vector capacity");
  if (input_lists.size() != n * L)
    throw std::runtime_error("state: ranking size does not match n*L");
  SparseState st;
  st.n = n; st.L = L; st.k = k; st.t = t; st.lval = lval;
  st.lists = std::move(input_lists);
  st.col_idx.resize(n * L);
  st.values.assign(n * L, 0.0f);
  st.nRef.assign(n, 0);
  st.rev_rowptr.assign(n + 1, 0);
  std::vector<uint32_t> row(L);
  for (size_t r = 0; r < n; ++r) {
    for (size_t j = 0; j < L; ++j) row[j] = st.lists[r * L + j];
    std::sort(row.begin(), row.end());
    for (size_t j = 0; j < L; ++j) st.col_idx[r * L + j] = row[j];
  }
  return st;
}

inline void k1_fillPos(SparseState& st) {
  std::fill(st.values.begin(), st.values.end(), 0.0f);
  for (size_t r = 0; r < st.n; ++r) {
    for (size_t pos = 0; pos < st.L; ++pos) {
      const uint32_t c = st.lists[r * st.L + pos];
      const double w = static_cast<double>(static_cast<int64_t>(st.lval) - static_cast<int64_t>(pos));
      const int64_t slot = find_slot(st, r, c);
      if (slot < 0) throw std::runtime_error("K1: column not in P (I1 violated)");
      acc_float(st.values[r * st.L + static_cast<size_t>(slot)], w);
      if (c == r) {
        acc_float(st.values[r * st.L + static_cast<size_t>(slot)], w);
      } else {
        const int64_t mirror = find_slot(st, c, static_cast<uint32_t>(r));
        if (mirror >= 0)
          acc_float(st.values[static_cast<size_t>(c) * st.L + static_cast<size_t>(mirror)], w);
      }
    }
  }
}

inline void insertion_sort_desc(float* a, uint32_t* rkrow, size_t length, uint32_t rk) {
  for (size_t j = 1; j < length; ++j) {
    const float keyA = a[j];
    const uint32_t keyR = rkrow[j];
    size_t i = j;
    while (i > 0 && a[i - 1] < keyA) {
      a[i] = a[i - 1]; rkrow[i] = rkrow[i - 1]; --i;
    }
    a[i] = keyA; rkrow[i] = keyR;
  }
  size_t self = 0;
  while (self < length && rkrow[self] != rk) ++self;
  if (self > 0 && self < length) {
    std::swap(rkrow[0], rkrow[self]); std::swap(a[0], a[self]);
  }
}

inline void k2_sortZero(SparseState& st) {
#if defined(_OPENMP)
  std::atomic<bool> invalid_slot(false);
#pragma omp parallel
  {
    std::vector<float> a(st.L);
#pragma omp for schedule(static)
    for (std::ptrdiff_t rr = 0; rr < static_cast<std::ptrdiff_t>(st.n); ++rr) {
      const size_t r = static_cast<size_t>(rr);
      for (size_t j = 0; j < st.L; ++j) {
        const uint32_t c = st.lists[r * st.L + j];
        const int64_t slot = find_slot(st, r, c);
        if (slot < 0) {
          invalid_slot.store(true, std::memory_order_relaxed);
          a[j] = 0.0f;
          continue;
        }
        a[j] = st.values[r * st.L + static_cast<size_t>(slot)];
        st.values[r * st.L + static_cast<size_t>(slot)] = 0.0f;
      }
      insertion_sort_desc(a.data(), &st.lists[r * st.L], st.L, static_cast<uint32_t>(r));
    }
  }
#else
  std::vector<float> a(st.L);
  for (size_t r = 0; r < st.n; ++r) {
    for (size_t j = 0; j < st.L; ++j) {
      const uint32_t c = st.lists[r * st.L + j];
      const int64_t slot = find_slot(st, r, c);
      if (slot < 0) throw std::runtime_error("K2: column not in P (I1 violated)");
      a[j] = st.values[r * st.L + static_cast<size_t>(slot)];
      st.values[r * st.L + static_cast<size_t>(slot)] = 0.0f;
    }
    insertion_sort_desc(a.data(), &st.lists[r * st.L], st.L, static_cast<uint32_t>(r));
  }
#endif
#if defined(_OPENMP)
  if (invalid_slot.load(std::memory_order_relaxed))
    throw std::runtime_error("K2: column not in P (I1 violated)");
#endif
}

inline void k3_cartProd(SparseState& st) {
  if (st.nRef.size() != st.n || st.rev_rowptr.size() != st.n + 1)
    throw std::runtime_error("K3: invalid reverse-index state");
  std::fill(st.nRef.begin(), st.nRef.end(), 0);
  for (size_t rk = 0; rk < st.n; ++rk)
    for (size_t j = 1; j < st.k; ++j) ++st.nRef[st.lists[rk * st.L + j]];
  st.rev_rowptr[0] = 0;
  for (size_t r = 0; r < st.n; ++r) {
    if (st.nRef[r] > st.rev_rowptr.max_size() - st.rev_rowptr[r])
      throw std::runtime_error("K3: reverse index size overflow");
    st.rev_rowptr[r + 1] = st.rev_rowptr[r] + st.nRef[r];
  }
  st.rev_src.assign(st.rev_rowptr[st.n], 0);
  st.rev_pos.assign(st.rev_rowptr[st.n], 0);
  std::vector<size_t> cursor(st.rev_rowptr.begin(), st.rev_rowptr.end() - 1);
  for (size_t rk = 0; rk < st.n; ++rk) {
    for (size_t j = 1; j < st.k; ++j) {
      const size_t nei = st.lists[rk * st.L + j];
      const size_t slot = cursor[nei]++;
      st.rev_src[slot] = rk; st.rev_pos[slot] = st.k - j;
    }
  }

  // Gather each destination row independently. For a given destination idx,
  // only sources whose top-(k-1) list contains idx (plus source idx itself)
  // can write that row. Rev rows are in increasing source-rank order; merge
  // source idx into that sequence to preserve the original rk/j/l addition
  // order for every cell, including the direct-then-mirror diagonal updates.
#if defined(_OPENMP)
  std::atomic<bool> invalid_reference(false);
#pragma omp parallel
  {
    std::vector<uint32_t> set(st.k);
#pragma omp for schedule(static)
    for (std::ptrdiff_t dd = 0; dd < static_cast<std::ptrdiff_t>(st.n); ++dd) {
      const size_t destination = static_cast<size_t>(dd);
#else
  std::vector<uint32_t> set(st.k);
  for (size_t destination = 0; destination < st.n; ++destination) {
#endif
    const size_t begin = st.rev_rowptr[destination];
    const size_t end = st.rev_rowptr[destination + 1];
    const auto apply_source = [&](size_t source, size_t destination_pos) {
      set[0] = static_cast<uint32_t>(source);
      for (size_t q = 1; q < st.k; ++q)
        set[q] = st.lists[source * st.L + q];
      if (destination_pos >= st.k || set[destination_pos] != destination) {
#if defined(_OPENMP)
        invalid_reference.store(true, std::memory_order_relaxed);
#endif
        return;
      }

      for (size_t j = 0; j < st.k; ++j) {
        if (j == destination_pos) {
          for (size_t l = 0; l < st.k; ++l) {
            const size_t column = set[l];
            const double score = static_cast<double>(st.k - j) *
                                 static_cast<double>(st.k - l);
            const int64_t direct = find_slot(st, destination,
                                             static_cast<uint32_t>(column));
            if (direct >= 0)
              acc_float(st.values[destination * st.L + static_cast<size_t>(direct)], score);
            if (l == destination_pos) {
              const int64_t mirror = find_slot(st, destination,
                                               static_cast<uint32_t>(set[j]));
              if (mirror >= 0)
                acc_float(st.values[destination * st.L + static_cast<size_t>(mirror)], score);
            }
          }
        } else {
          const size_t column = set[j];
          const double score = static_cast<double>(st.k - j) *
                               static_cast<double>(st.k - destination_pos);
          const int64_t mirror = find_slot(st, destination,
                                           static_cast<uint32_t>(column));
          if (mirror >= 0)
            acc_float(st.values[destination * st.L + static_cast<size_t>(mirror)], score);
        }
      }
    };

    size_t ref = begin;
    bool self_done = false;
    while (ref < end) {
      const size_t source = st.rev_src[ref];
      if (!self_done && destination < source) {
        apply_source(destination, 0);
        self_done = true;
      }
      const size_t destination_pos = st.k - st.rev_pos[ref];
      apply_source(source, destination_pos);
      ++ref;
    }
    if (!self_done) apply_source(destination, 0);
#if defined(_OPENMP)
    }
  }
#else
  }
#endif
#if defined(_OPENMP)
  if (invalid_reference.load(std::memory_order_relaxed))
    throw std::runtime_error("K3: reverse reference position mismatch");
#endif
}

inline void k4_reverseCartProd(SparseState& st) {
  if (st.rev_rowptr.size() != st.n + 1 || st.rev_src.size() != st.rev_pos.size())
    throw std::runtime_error("K4: invalid Rev CSR");
  for (size_t rk = 0; rk < st.n; ++rk) {
    const size_t begin = st.rev_rowptr[rk], end = st.rev_rowptr[rk + 1];
    for (size_t j = begin; j < end; ++j) {
      const size_t idx = st.rev_src[j];
      for (size_t l = begin; l < end; ++l) {
        const size_t idy = st.rev_src[l];
        const double score = static_cast<double>(st.rev_pos[j]) * static_cast<double>(st.rev_pos[l]);
        const int64_t direct = find_slot(st, idx, static_cast<uint32_t>(idy));
        if (direct >= 0) acc_float(st.values[idx * st.L + static_cast<size_t>(direct)], score);
        const int64_t mirror = find_slot(st, idy, static_cast<uint32_t>(idx));
        if (mirror >= 0) acc_float(st.values[idy * st.L + static_cast<size_t>(mirror)], score);
      }
    }
  }
}

inline void k5_cleanRef(SparseState& st) {
  std::fill(st.nRef.begin(), st.nRef.end(), 0);
  st.rev_src.clear(); st.rev_pos.clear(); st.rev_rowptr.assign(st.n + 1, 0);
}

inline void k6_sort(SparseState& st) {
#if defined(_OPENMP)
  std::atomic<bool> invalid_slot(false);
#pragma omp parallel
  {
    std::vector<float> a(st.L);
#pragma omp for schedule(static)
    for (std::ptrdiff_t rr = 0; rr < static_cast<std::ptrdiff_t>(st.n); ++rr) {
      const size_t r = static_cast<size_t>(rr);
      for (size_t j = 0; j < st.L; ++j) {
        const uint32_t c = st.lists[r * st.L + j];
        const int64_t slot = find_slot(st, r, c);
        if (slot < 0) {
          invalid_slot.store(true, std::memory_order_relaxed);
          a[j] = 0.0f;
          continue;
        }
        a[j] = st.values[r * st.L + static_cast<size_t>(slot)];
      }
      insertion_sort_desc(a.data(), &st.lists[r * st.L], st.L, static_cast<uint32_t>(r));
    }
  }
  if (invalid_slot.load(std::memory_order_relaxed))
    throw std::runtime_error("K6: column not in P (I1 violated)");
#else
  std::vector<float> a(st.L);
  for (size_t r = 0; r < st.n; ++r) {
    for (size_t j = 0; j < st.L; ++j) {
      const uint32_t c = st.lists[r * st.L + j];
      const int64_t slot = find_slot(st, r, c);
      if (slot < 0) throw std::runtime_error("K6: column not in P (I1 violated)");
      a[j] = st.values[r * st.L + static_cast<size_t>(slot)];
    }
    insertion_sort_desc(a.data(), &st.lists[r * st.L], st.L, static_cast<uint32_t>(r));
  }
#endif
}

}  // namespace cprr

#endif  // CPRR_SPARSE_CORE_HPP