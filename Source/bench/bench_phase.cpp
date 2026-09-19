// bench_phase.cpp
//
// Phase 0 profiling driver for CSRMatrix
// (Source/src/Core/hpp/SparseMatrix.hpp).
//
// Runs the production pipeline on synthetic matrices shaped like the UDLF
// workloads and prints machine-parseable lines:
//
//   RUN,<mode>,<workload>,<omp_threads>,<reps>,<phase_profile_flag>
//   REP,<i>,<total_ms>,<nnz>,<checksum>          one per repetition
//   PHASEPROFILE,...                             per-phase CSV (only when
//                                                built with -DPHASE_PROFILE)
//   DONE,<workload>,<vmhwm_kb>,<ru_maxrss_kb>
//
// Aggregation (median over reps, rep 0 discarded) is done offline by
// Source/bench/aggregate_phase.py.
//
// Workloads (uniform random k-sparse; kNN-clustered columns are a known
// limitation -- Phase 0 uses these only to exercise both accumulator
// branches and the heavy-prune regime):
//   w1: CPRR-like, DENSE accumulator branch (B.cols/bound = 50000/400 = 125
//       < ADAPTIVE_RATIO 200), prune limit 50.
//   w2: CPRR-like, HASH accumulator branch   (B.cols/bound = 1e6/400 = 2500),
//       prune limit 50.
//   w3: LHRR-like heavy prune: bound ~= ra*rb = 62500/row, limit 1000
//       (dense branch; nth_element dominates).
//
// Usage:  bench_phase <mult|pipe> <w1|w2|w3> [reps=7]
//   mult: reps x multiply_and_prune(A, B, limit)
//   pipe: reps x { WT=transpose(A); MA=mult(WT,A,limit);
//                  MB=mult(A,WT,limit); C=merge(MA,MB,plus) }  (CPRR iteration)
// Threads come from OMP_NUM_THREADS.
//
// Build (WSL, from repo root):
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp \
//       -o /tmp/bench_phase -ltbb
//   add -DPHASE_PROFILE for the per-phase report.

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

struct Workload {
    const char* name;
    idx_t rows_a;   // rows of A
    idx_t inner;    // cols of A == rows of B
    idx_t cols_b;   // cols of B
    int ra, rb;     // nnz per row of A / B
    size_t limit;   // prune limit
};

static const Workload WORKLOADS[] = {
    // name  rows_a  inner   cols_b     ra   rb  limit
    {"w1",   50000, 100000,   50000,    20,  20,   50},
    {"w2",   50000, 100000, 1000000,    20,  20,   50},
    {"w3",   20000, 100000,  100000,   250, 250, 1000},
};

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

// Random k-sparse CSR, distinct sorted columns per row (uniform columns).
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

// Deterministic checksum (storage order) -- same definition as bench_e2e.
static double checksum(const M& m) {
    double s = 0.0;
    for (idx_t r = 0; r < m.rows(); ++r)
        m.for_each_in_row(r, [&](idx_t, float v) { s += (double)v; });
    return s;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <mult|pipe> <w1|w2|w3> [reps=7]\n", argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    std::string wl = argv[2];
    int reps = (argc > 3) ? std::atoi(argv[3]) : 7;
    if (reps < 1) reps = 1;

    const Workload* W = nullptr;
    for (const Workload& w : WORKLOADS) if (wl == w.name) W = &w;
    if (!W) { std::fprintf(stderr, "unknown workload %s\n", wl.c_str()); return 2; }

    bool pp_flag =
#if defined(PHASE_PROFILE)
        true;
#else
        false;
#endif

    std::printf("RUN,%s,%s,%d,%d,%d\n", mode.c_str(), W->name,
                omp_get_max_threads(), reps, (int)pp_flag);

    std::mt19937 rngA(42), rngB(1000 + (int)W->cols_b);
    M A = random_csr(W->rows_a, W->inner, W->ra, rngA);
    M B = random_csr(W->inner, W->cols_b, W->rb, rngB);
    // pipe mode replicates a CPRR iteration, which needs a SQUARE W (N x N);
    // reuse rows_a as N and ra as k. (A/B above are only for mult mode.)
    M Wsq = (mode == "pipe")
        ? random_csr(W->rows_a, W->rows_a, W->ra, rngA) : M();

    M result;
    for (int rep = 0; rep < reps; ++rep) {
#ifdef PHASE_PROFILE
        phprof::reset();
#endif
        auto t0 = std::chrono::steady_clock::now();
        if (mode == "pipe") {
            M WT = Wsq.transpose();
            M MA = M::multiply_and_prune(WT, Wsq, W->limit);
            M MB = M::multiply_and_prune(Wsq, WT, W->limit);
            result = M::merge(MA, MB, [](float a, float b) { return a + b; });
        } else {
            result = M::multiply_and_prune(A, B, W->limit);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("REP,%d,%.3f,%zu,%.6f\n", rep, ms, result.nnz(), checksum(result));
        std::fflush(stdout);
#ifdef PHASE_PROFILE
        phprof::report(stdout);
#endif
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef PHASE_PROFILE
    long hwm = phprof::vmhwm_kb();
#else
    long hwm = -1;
#endif
    std::printf("DONE,%s,%ld,%ld\n", W->name, hwm, ru.ru_maxrss);
    return 0;
}

