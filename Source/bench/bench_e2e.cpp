// bench_e2e.cpp
//
// End-to-end benchmark of the REAL CSRMatrix::multiply_and_prune pipeline
// (accumulator + BlockAllocator + parallel sort + CSR build) against a
// faithful replica of the original dense-only kernel, over the same
// production merge path (build_from_partial_results).
//
// Usage: bench_e2e <dense|adaptive> <N_cols_B>
// Prints:  <variant> <N> <time_ms> <peak_rss_KB> <nnz>
// Each (variant, N) pair runs in its own process so ru_maxrss is per-variant.
//
// Build (WSL, from repo root):
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_e2e.cpp \
//       -o /tmp/bench_e2e -ltbb

#include "Core/hpp/SparseMatrix.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <sys/resource.h>
#include <tuple>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

// Build a CSR matrix through the production Row-Split API.
static M build_csr(idx_t rows, idx_t cols,
                   const std::vector<std::tuple<idx_t, idx_t, float>>& in) {
    M m(rows, cols);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();

    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
    size_t chunk = (rows + num_threads - 1) / num_threads;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        idx_t r_start = (idx_t)std::min<size_t>(rows, tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(rows, (tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
        for (size_t t = 0; t < in.size(); ++t) {
            idx_t r = std::get<0>(in[t]);
            if (r >= r_start && r < r_end) {
                Triplet* tr = allocator.allocate();
                tr->row = r; tr->col = std::get<1>(in[t]);
                tr->value = std::get<2>(in[t]); tr->is_set = false;
            }
        }
    }
    M::build_from_partial_results(m, allocators, ranges);
    return m;
}

static std::tuple<idx_t, idx_t, int> gen_params() { return {50000, 100000, 20}; }

// Random k-sparse CSR (distinct sorted columns per row).
static M random_csr(idx_t rows, idx_t cols, int nnz_per_row, std::mt19937& rng) {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)rows * nnz_per_row);
    std::uniform_real_distribution<float> valdist(-1.0f, 1.0f);
    for (idx_t r = 0; r < rows; ++r) {
        std::vector<idx_t> cols_row;
        int want = std::min<int>(nnz_per_row, (int)cols);
        std::uniform_int_distribution<idx_t> sample(0, cols - 1);
        while ((int)cols_row.size() < want) {
            idx_t c = sample(rng);
            if (std::find(cols_row.begin(), cols_row.end(), c) == cols_row.end())
                cols_row.push_back(c);
        }
        std::sort(cols_row.begin(), cols_row.end());
        for (idx_t c : cols_row) {
            float v = valdist(rng);
            trips.emplace_back(r, c, v == 0.0f ? 0.5f : v);
        }
    }
    return build_csr(rows, cols, trips);
}

// Faithful replica of the ORIGINAL dense-only kernel (dense_acc + marker +
// fresh active_cols per row + nth_element + resize), emitting through the
// same production merge path.
static M multiply_and_prune_original(const M& A, const M& B, size_t limit) {
    M C(A.rows(), B.cols(), 0);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();

    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;

        size_t chunk = (A.rows() + num_threads - 1) / num_threads;
        size_t r_start = std::min((size_t)A.rows(), (size_t)tid * chunk);
        size_t r_end   = std::min((size_t)A.rows(), (size_t)(tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};

        std::vector<float> dense_acc(B.cols(), 0.0f);
        std::vector<idx_t> marker(B.cols(), (idx_t)-1);

        for (size_t i = r_start; i < r_end; ++i) {
            std::vector<size_t> active_cols;
            A.for_each_in_row((idx_t)i, [&](idx_t k, float val_a) {
                B.for_each_in_row(k, [&](idx_t j, float val_b) {
                    if (marker[j] != (idx_t)i) {
                        marker[j] = (idx_t)i;
                        dense_acc[j] = 0.0f;
                        active_cols.push_back(j);
                    }
                    dense_acc[j] += val_a * val_b;
                });
            });
            if (active_cols.size() > limit) {
                std::nth_element(active_cols.begin(), active_cols.begin() + limit,
                                 active_cols.end(),
                    [&](size_t a, size_t b) { return std::abs(dense_acc[a]) > std::abs(dense_acc[b]); });
                active_cols.resize(limit);
            }
            for (size_t col : active_cols) {
                if (std::abs(dense_acc[col]) > 1e-9f) {
                    Triplet* t = allocator.allocate();
                    t->row = (idx_t)i; t->col = (idx_t)col;
                    t->value = dense_acc[col]; t->is_set = false;
                }
            }
        }
    }
    M::build_from_partial_results(C, allocators, ranges);
    return C;
}

// Deterministic checksum of the result (values only, storage order).
static double checksum(const M& m) {
    double s = 0.0;
    for (idx_t r = 0; r < m.rows(); ++r)
        m.for_each_in_row(r, [&](idx_t, float v) { s += (double)v; });
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <dense|adaptive> <N_cols_B>\n", argv[0]);
        return 2;
    }
    std::string variant = argv[1];
    idx_t N = (idx_t)std::strtoul(argv[2], nullptr, 10);

    const idx_t M_rows = 50000, K = 100000;
    const int RA = 20, RB = 20;
    const size_t LIMIT = 50;

    std::mt19937 rngA(42), rngB(1000 + N);
    M A = random_csr(M_rows, K, RA, rngA);
    M B = random_csr(K, N, RB, rngB);

    // Warm-up (untimed, small subset is not representative; run full once).
    M result;
    if (variant == "dense") result = multiply_and_prune_original(A, B, LIMIT);
    else                    result = M::multiply_and_prune(A, B, LIMIT);

    // Timed: best of 2.
    double best_ms = 1e18;
    for (int rep = 0; rep < 2; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        result = (variant == "dense") ? multiply_and_prune_original(A, B, LIMIT)
                                      : M::multiply_and_prune(A, B, LIMIT);
        auto t1 = std::chrono::steady_clock::now();
        best_ms = std::min(best_ms,
                           std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    std::printf("%-9s N=%-9u time_ms=%-9.1f peak_rss_KB=%-10zu nnz=%-9zu checksum=%.6f\n",
                variant.c_str(), (unsigned)N, best_ms, (size_t)ru.ru_maxrss,
                result.nnz(), checksum(result));
    return 0;
}

