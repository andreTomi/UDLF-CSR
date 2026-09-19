// bench_accumulator.cpp
//
// Standalone validation/benchmark for the dense-vs-hash row accumulator
// used in CSRMatrix::multiply_and_prune (see Source/src/Core/hpp/SparseMatrix.hpp).
// Isolates just the accumulator strategy so you can:
//   1. Confirm correctness (dense and hash produce identical pruned output,
//      modulo fp summation-order noise).
//   2. Measure the crossover point between the two on YOUR hardware and
//      sparsity profile, and re-tune ADAPTIVE_RATIO in SparseMatrix.hpp
//      accordingly.
//
// Build (WSL, from repo root):
//   g++ -O3 -march=native -std=c++17 Source/bench/bench_accumulator.cpp \
//       -o /tmp/bench_accumulator
//
// No OpenMP/BlockAllocator/<execution> dependency -- deliberately decoupled
// from the rest of the project so it builds anywhere.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <unordered_set>
#include <vector>

using idx_t = uint32_t;

struct CSR {
    idx_t rows = 0, cols = 0;
    std::vector<idx_t> row_ptr;
    std::vector<idx_t> col_idx;
    std::vector<double> values;
};

// rows x cols matrix, `nnz_per_row` distinct random columns per row.
static CSR make_random_csr(idx_t rows, idx_t cols, int nnz_per_row, std::mt19937_64& rng) {
    CSR m;
    m.rows = rows;
    m.cols = cols;
    m.row_ptr.resize(rows + 1, 0);
    m.col_idx.reserve((size_t)rows * nnz_per_row);
    m.values.reserve((size_t)rows * nnz_per_row);
    std::uniform_real_distribution<double> valdist(0.1, 1.0);
    std::uniform_int_distribution<idx_t> coldist(0, cols - 1);

    std::vector<idx_t> buf;
    buf.reserve(nnz_per_row * 2);
    std::unordered_set<idx_t> chosen;
    chosen.reserve(nnz_per_row * 2);

    for (idx_t r = 0; r < rows; ++r) {
        chosen.clear();
        int want = std::min<int>(nnz_per_row, cols);
        while ((int)chosen.size() < want) chosen.insert(coldist(rng));
        buf.assign(chosen.begin(), chosen.end());
        std::sort(buf.begin(), buf.end());
        for (idx_t c : buf) {
            m.col_idx.push_back(c);
            m.values.push_back(valdist(rng));
        }
        m.row_ptr[r + 1] = m.row_ptr[r] + (idx_t)buf.size();
    }
    return m;
}

// ---------------------------------------------------------------------------
// Baseline: dense SPA accumulator (== current dense_acc[]/marker[] in the file)
// ---------------------------------------------------------------------------
struct DenseAcc {
    std::vector<double> val;
    std::vector<int64_t> marker;
    explicit DenseAcc(size_t n) : val(n, 0.0), marker(n, -1) {}
};

static void multiply_row_dense(idx_t row_id, const CSR& A, const CSR& B, DenseAcc& acc,
                                std::vector<idx_t>& active_cols,
                                std::vector<std::pair<idx_t, double>>& out) {
    active_cols.clear();
    for (idx_t ia = A.row_ptr[row_id]; ia < A.row_ptr[row_id + 1]; ++ia) {
        idx_t k = A.col_idx[ia];
        double va = A.values[ia];
        for (idx_t ib = B.row_ptr[k]; ib < B.row_ptr[k + 1]; ++ib) {
            idx_t j = B.col_idx[ib];
            if (acc.marker[j] != (int64_t)row_id) {
                acc.marker[j] = (int64_t)row_id;
                acc.val[j] = 0.0;
                active_cols.push_back(j);
            }
            acc.val[j] += va * B.values[ib];
        }
    }
    out.clear();
    out.reserve(active_cols.size());
    for (idx_t c : active_cols) out.emplace_back(c, acc.val[c]);
}

// ---------------------------------------------------------------------------
// Proposed: hash accumulator, capacity O(k^2) (fixed once per thread/run)
// ---------------------------------------------------------------------------
struct HashAcc {
    static constexpr idx_t EMPTY = (idx_t)-1;
    std::vector<idx_t> slot_col;
    std::vector<double> slot_val;
    std::vector<uint32_t> used;
    size_t mask = 0;

    static size_t next_pow2(size_t x) {
        size_t p = 1;
        while (p < x) p <<= 1;
        return p;
    }
    void ensure_capacity(size_t bound) {
        size_t need = next_pow2(std::max<size_t>(4, bound * 2));  // load factor <= 50%
        if (need > slot_col.size()) {
            slot_col.assign(need, EMPTY);
            slot_val.assign(need, 0.0);
            mask = need - 1;
        }
    }
    inline void begin_row() {
        for (auto s : used) slot_col[s] = EMPTY;  // O(k), not O(capacity)
        used.clear();
    }
    static inline size_t mix(idx_t x) {
        uint64_t h = x;
        h ^= h >> 16;
        h *= 0x85ebca6bULL;
        h ^= h >> 13;
        h *= 0xc2b2ae35ULL;
        h ^= h >> 16;
        return (size_t)h;
    }
    inline void accumulate(idx_t col, double v) {
        size_t h = mix(col) & mask;
        while (slot_col[h] != EMPTY && slot_col[h] != col) h = (h + 1) & mask;
        if (slot_col[h] == EMPTY) {
            slot_col[h] = col;
            slot_val[h] = v;
            used.push_back((uint32_t)h);
        } else {
            slot_val[h] += v;
        }
    }
};

static void multiply_row_hash(idx_t row_id, const CSR& A, const CSR& B, HashAcc& acc,
                               std::vector<std::pair<idx_t, double>>& out) {
    acc.begin_row();
    for (idx_t ia = A.row_ptr[row_id]; ia < A.row_ptr[row_id + 1]; ++ia) {
        idx_t k = A.col_idx[ia];
        double va = A.values[ia];
        for (idx_t ib = B.row_ptr[k]; ib < B.row_ptr[k + 1]; ++ib) {
            acc.accumulate(B.col_idx[ib], va * B.values[ib]);
        }
    }
    out.clear();
    out.reserve(acc.used.size());
    for (auto s : acc.used) out.emplace_back(acc.slot_col[s], acc.slot_val[s]);
}

static size_t compute_max_bound(const CSR& A, const CSR& B) {
    size_t maxb = 0;
    for (idx_t r = 0; r < A.rows; ++r) {
        size_t b = 0;
        for (idx_t ia = A.row_ptr[r]; ia < A.row_ptr[r + 1]; ++ia) {
            idx_t k = A.col_idx[ia];
            b += (B.row_ptr[k + 1] - B.row_ptr[k]);
        }
        maxb = std::max(maxb, b);
    }
    return maxb;
}

static void prune(std::vector<std::pair<idx_t, double>>& v, size_t limit) {
    if (v.size() > limit) {
        std::nth_element(v.begin(), v.begin() + limit, v.end(), [](auto& a, auto& b) {
            return std::abs(a.second) > std::abs(b.second);
        });
        v.resize(limit);
    }
}

int main(int argc, char** argv) {
    const idx_t K = 100000;   // shared dim: A.cols_ == B.rows_  (fixed)
    const idx_t M = 50000;    // A.rows_                          (fixed)
    const int RA = 20;        // nnz per row of A                 (fixed)
    const int RB = 20;        // nnz per row of B                 (fixed)
    const size_t LIMIT = 50;  // prune limit, same semantics as multiply_and_prune

    std::vector<idx_t> N_values = {10000, 100000, 200000, 500000, 1000000, 2000000, 10000000, 50000000};
    // Optional: pass N values as argv (e.g. ./bench_accumulator 100000 200000)
    // so long runs can be executed one N at a time.
    if (argc > 1) {
        N_values.clear();
        for (int i = 1; i < argc; ++i)
            N_values.push_back((idx_t)std::strtoul(argv[i], nullptr, 10));
    }

    std::mt19937_64 rng(42);
    CSR A = make_random_csr(M, K, RA, rng);

    std::cout << "M=" << M << " K=" << K << " ra=" << RA << " rb=" << RB
              << " limit=" << LIMIT << "\n";
    std::cout << std::left << std::setw(12) << "N(cols B)" << std::setw(16) << "dense_ms"
              << std::setw(16) << "hash_ms" << std::setw(10) << "speedup" << "correctness\n";

    for (idx_t N : N_values) {
        std::mt19937_64 rngB(1000 + N);
        CSR B = make_random_csr(K, N, RB, rngB);

        size_t max_bound = compute_max_bound(A, B);

        DenseAcc dacc(N);
        HashAcc hacc;
        hacc.ensure_capacity(max_bound);

        std::vector<idx_t> active_cols;
        std::vector<std::pair<idx_t, double>> out_dense, out_hash;

        // ---- Pass 1 (untimed): correctness check, full per-row comparison ----
        bool all_match = true;
        double max_abs_diff = 0.0;
        for (idx_t r = 0; r < M; ++r) {
            multiply_row_dense(r, A, B, dacc, active_cols, out_dense);
            multiply_row_hash(r, A, B, hacc, out_hash);
            prune(out_dense, LIMIT);
            prune(out_hash, LIMIT);
            std::sort(out_dense.begin(), out_dense.end());
            std::sort(out_hash.begin(), out_hash.end());
            if (out_dense.size() != out_hash.size()) {
                all_match = false;
                continue;
            }
            for (size_t i = 0; i < out_dense.size(); ++i) {
                if (out_dense[i].first != out_hash[i].first) all_match = false;
                max_abs_diff = std::max(max_abs_diff, std::abs(out_dense[i].second - out_hash[i].second));
            }
        }

        // ---- Pass 2 (timed): symmetric workload, no result storage/copies ----
        volatile double checksum_dense = 0.0, checksum_hash = 0.0;

        auto t0 = std::chrono::steady_clock::now();
        for (idx_t r = 0; r < M; ++r) {
            multiply_row_dense(r, A, B, dacc, active_cols, out_dense);
            prune(out_dense, LIMIT);
            double s = 0.0;
            for (auto& p : out_dense) s += p.second;
            checksum_dense += s;
        }
        auto t1 = std::chrono::steady_clock::now();

        for (idx_t r = 0; r < M; ++r) {
            multiply_row_hash(r, A, B, hacc, out_hash);
            prune(out_hash, LIMIT);
            double s = 0.0;
            for (auto& p : out_hash) s += p.second;
            checksum_hash += s;
        }
        auto t2 = std::chrono::steady_clock::now();

        double dense_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double hash_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

        std::cout << std::left << std::setw(12) << N << std::setw(16) << dense_ms
                  << std::setw(16) << hash_ms << std::setw(10) << (dense_ms / hash_ms)
                  << (all_match ? "OK" : "MISMATCH") << "  (max|diff|=" << max_abs_diff
                  << ", max_bound=" << max_bound << ")\n";
    }
    return 0;
}

