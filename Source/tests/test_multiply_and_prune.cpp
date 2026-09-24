// test_multiply_and_prune.cpp
//
// Correctness suite for the adaptive dense/hash row accumulator in
// CSRMatrix::multiply_and_prune (Source/src/Core/hpp/SparseMatrix.hpp).
//
// Build (WSL, from repo root):
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src \
//       Source/tests/test_multiply_and_prune.cpp -o /tmp/test_map
//
// Exit code 0 = all tests passed.

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <tuple>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

static int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Build a CSR matrix through the same public Row-Split API the methods use
// (PaddedAllocator + build_from_partial_results). Duplicate (row,col) inputs
// are summed; output is row/col sorted by the builder.
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

// Random k-sparse CSR (distinct sorted columns per row, random values).
static M random_csr(idx_t rows, idx_t cols, int nnz_per_row, std::mt19937& rng) {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
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

// Aggregate a matrix into a (row,col)->value map via public readers.
static std::map<std::pair<idx_t, idx_t>, float> to_map(const M& m) {
    std::map<std::pair<idx_t, idx_t>, float> out;
    for (idx_t r = 0; r < m.rows(); ++r)
        m.for_each_in_row(r, [&](idx_t c, float v) { out[{r, c}] += v; });
    return out;
}

// Independent reference: fresh std::map per row, same iteration order as the
// kernel (A storage order, then B storage order) so float sums are bit-exact,
// same prune rule (nth_element by |v| desc, then |v| > 1e-9 filter).
static M reference_multiply(const M& A, const M& B, size_t limit) {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    std::map<idx_t, float> acc;      // col -> value (fresh per row)
    std::vector<idx_t> active;       // first-touch order
    for (idx_t i = 0; i < A.rows(); ++i) {
        acc.clear(); active.clear();
        A.for_each_in_row(i, [&](idx_t k, float va) {
            B.for_each_in_row(k, [&](idx_t j, float vb) {
                auto it = acc.find(j);
                if (it == acc.end()) { acc[j] = va * vb; active.push_back(j); }
                else it->second += va * vb;
            });
        });
        if (active.size() > limit) {
            std::nth_element(active.begin(), active.begin() + limit, active.end(),
                [&](idx_t a, idx_t b) { return std::abs(acc[a]) > std::abs(acc[b]); });
            active.resize(limit);
        }
        for (idx_t c : active) {
            float v = acc[c];
            if (std::abs(v) > 1e-9f) trips.emplace_back(i, c, v);
        }
    }
    return build_csr(A.rows(), B.cols(), trips);
}

// Structural + bit-exact value comparison through public readers.
static bool same_result(const M& a, const M& b) {
    if (a.rows() != b.rows() || a.cols() != b.cols() || a.nnz() != b.nnz()) return false;
    std::map<std::pair<idx_t, idx_t>, float> ma = to_map(a);
    std::map<std::pair<idx_t, idx_t>, float> mb = to_map(b);
    if (ma.size() != mb.size()) return false;
    auto ia = ma.begin();
    auto ib = mb.begin();
    for (; ia != ma.end(); ++ia, ++ib) {
        if (ia->first != ib->first || ia->second != ib->second) return false;
    }
    return true;
}

// Force branch selection exactly like multiply_and_prune does (ratio 200).
static bool would_use_hash(const M& A, const M& B) {
    size_t max_bound = 0;
    for (idx_t i = 0; i < A.rows(); ++i) {
        size_t bound = 0;
        A.for_each_in_row(i, [&](idx_t k, float) { bound += B.row_nnz(k); });
        max_bound = std::max(max_bound, bound);
    }
    return (size_t)B.cols() > 200 * std::max<size_t>(max_bound, 1);
}

static void run_case(const char* name, const M& A, const M& B, size_t limit,
                     bool expect_hash_branch) {
    std::printf("== %s (expect hash branch: %s)\n", name,
                expect_hash_branch ? "yes" : "no");
    CHECK(would_use_hash(A, B) == expect_hash_branch,
          "branch selection does not match test expectation");
    M got = M::multiply_and_prune(A, B, limit);
    M ref = reference_multiply(A, B, limit);
    CHECK(same_result(got, ref), "structure or bit-exact values mismatch");
}

int main() {
    std::printf("Test suite: adaptive multiply_and_prune (threads=%d)\n",
                omp_get_max_threads());

    // ---- A1: hand-checked small case, dense branch ----
    {
        std::vector<std::tuple<idx_t, idx_t, float>> ta = {
            {0, 0, 1.0f}, {0, 1, 2.0f}, {1, 2, 3.0f}};
        std::vector<std::tuple<idx_t, idx_t, float>> tb = {
            {0, 0, 1.0f}, {0, 2, 5.0f}, {1, 1, 4.0f}, {2, 3, 2.0f}, {2, 0, 1.0f}};
        M A = build_csr(2, 3, ta);
        M B = build_csr(3, 4, tb);
        // Row 0: k=0 -> j=0(+1),j=2(+5); k=1 -> j=1(+8). Row 1: k=2 -> j=0(+3),j=3(+6).
        M got = M::multiply_and_prune(A, B, 10);
        std::map<std::pair<idx_t, idx_t>, float> g = to_map(got);
        std::printf("== A1 hand-checked (dense branch)\n");
        CHECK(g.size() == 5, "A1: 5 live entries expected");
        CHECK((g.count({0, 0}) && g[{0, 0}] == 1.0f), "A1: C(0,0)==1");
        CHECK((g.count({0, 1}) && g[{0, 1}] == 8.0f), "A1: C(0,1)==8");
        CHECK((g.count({0, 2}) && g[{0, 2}] == 5.0f), "A1: C(0,2)==5");
        CHECK((g.count({1, 0}) && g[{1, 0}] == 3.0f), "A1: C(1,0)==3");
        CHECK((g.count({1, 3}) && g[{1, 3}] == 6.0f), "A1: C(1,3)==6");
    }

    // ---- A2: random, dense branch, vs independent reference ----
    {
        std::mt19937 rng(7);
        M A = random_csr(200, 300, 12, rng);
        M B = random_csr(300, 1000, 15, rng);   // bound ~180, N=1000 -> dense
        run_case("A2 random dense-branch vs reference", A, B, 20, false);
    }

    // ---- A3: random, hash branch, vs independent reference ----
    {
        std::mt19937 rng(8);
        M A = random_csr(500, 400, 15, rng);
        M B = random_csr(400, 300000, 15, rng); // bound ~225, N=300k -> hash
        run_case("A3 random hash-branch vs reference", A, B, 30, true);
    }

    // ---- A5: prune ties (invariants only: count and magnitude) ----
    {
        std::printf("== A5 prune ties\n");
        std::vector<std::tuple<idx_t, idx_t, float>> ta = {{0, 0, 1.0f}, {0, 1, 1.0f}};
        std::vector<std::tuple<idx_t, idx_t, float>> tb = {
            {0, 0, 1.0f}, {0, 1, 1.0f}, {1, 2, 1.0f}, {1, 3, 1.0f}};
        M A = build_csr(1, 2, ta);
        M B = build_csr(2, 4, tb);
        M got = M::multiply_and_prune(A, B, 2);
        CHECK(got.nnz() == 2, "A5: exactly `limit` entries emitted");
        std::map<std::pair<idx_t, idx_t>, float> g5 = to_map(got);
        for (auto& kv : g5)
            CHECK(std::abs(kv.second) == 1.0f, "A5: tied entry must have |v|=1");
    }

    // ---- A6: edge cases ----
    {
        std::printf("== A6 edge cases\n");
        std::mt19937 rng(11);
        {   // limit = 0 -> empty result
            M A = random_csr(10, 10, 3, rng);
            M B = random_csr(10, 10, 3, rng);
            M got = M::multiply_and_prune(A, B, 0);
            CHECK(got.nnz() == 0, "A6: limit=0 produces empty result");
        }
        {   // limit > touched -> nothing pruned
            M A = random_csr(10, 20, 3, rng);
            M B = random_csr(20, 30, 3, rng);
            run_case("A6 limit>touched", A, B, 1000, false);
        }
        {   // near-zero accumulation dropped
            M A = build_csr(1, 1, {{0, 0, 1e-20f}});
            M B = build_csr(1, 1, {{0, 0, 1e-20f}});
            M got = M::multiply_and_prune(A, B, 10);
            CHECK(got.nnz() == 0, "A6: near-zero accumulation dropped");
        }
        {   // empty rows in A and B
            M A = build_csr(2, 2, {{1, 0, 2.0f}});   // row 0 empty
            M B = build_csr(2, 2, {{0, 1, 5.0f}});   // row 1 empty
            M got = M::multiply_and_prune(A, B, 10);
            CHECK(got.nnz() == 1, "A6: one live entry with empty rows");
            CHECK(got.get(1, 1) == 10.0f, "A6: empty-rows value C(1,1)==10");
        }
        {   // collision-heavy: all B rows share the same columns (hash branch)
            std::vector<std::tuple<idx_t, idx_t, float>> tb;
            for (idx_t k = 0; k < 60; ++k) tb.emplace_back(k, 7, 1.0f);
            std::vector<std::tuple<idx_t, idx_t, float>> ta;
            for (idx_t i = 0; i < 40; ++i) ta.emplace_back(i, 0, 2.0f);
            M A = build_csr(40, 60, ta);
            M B = build_csr(60, 8, tb);
            M got = M::multiply_and_prune(A, B, 5);
            M ref = reference_multiply(A, B, 5);
            CHECK(got.nnz() == ref.nnz() && got.nnz() == 40,
                  "A6: collision-heavy emits one entry per row");
            CHECK(same_result(got, ref), "A6: collision-heavy matches reference");
        }
        {   // A.rows_ not divisible by thread count, hash branch
            M A = random_csr(37, 50, 5, rng);
            M B = random_csr(50, 400000, 4, rng);
            run_case("A6 non-divisible rows, hash branch", A, B, 8, true);
        }
    }



    // ---- A7: saturation stress (leaked-slot regression shape) ----
    {
        std::printf("== A7 saturation stress\n");
        // Bounds around powers of two with aggressive pruning. If the
        // slot-leak bug regressed, probing degenerates / asserts fire.
        for (size_t bound : {15u, 16u, 17u, 255u, 256u, 257u}) {
            std::vector<std::tuple<idx_t, idx_t, float>> tb, ta;
            for (idx_t k = 0; k < (idx_t)bound; ++k) tb.emplace_back(k, k, 1.0f);
            for (idx_t i = 0; i < (idx_t)bound; ++i) ta.emplace_back(i, i, 0.5f);
            M B = build_csr((idx_t)bound, (idx_t)bound, tb);
            M A = build_csr((idx_t)bound, (idx_t)bound, ta);
            size_t limit = bound / 2;
            M got = M::multiply_and_prune(A, B, limit);
            CHECK(got.nnz() == bound, "A7: one pruned entry per row");
            bool ok = true;
            for (auto& kv : to_map(got))
                if (kv.second != 0.5f) ok = false;
            CHECK(ok, "A7: pruned entries carry accumulated value");
        }
        // Many consecutive rows re-touching a shifting window, wide B (hash
        // branch) and heavy pruning. Rows only touch low columns so B stays
        // sparse while B.cols_ is wide -> identical math, hash branch active.
        {
            idx_t rows = 2000, span = 1024;
            std::mt19937 rng(99);
            std::vector<std::tuple<idx_t, idx_t, float>> tb, ta;
            for (idx_t k = 0; k < span; ++k) tb.emplace_back(k, k, 1.0f);
            std::uniform_int_distribution<idx_t> kd(0, span - 1);
            for (idx_t i = 0; i < rows; ++i)
                for (int t = 0; t < 20; ++t)
                    ta.emplace_back(i, kd(rng), 1.0f);
            M B2 = build_csr(span, 300000, tb);
            M A2 = build_csr(rows, span, ta);
            M got = M::multiply_and_prune(A2, B2, 5);
            CHECK(got.nnz() == (size_t)rows * 5, "A7: shifting-window emits limit/row");
        }
    }

    // ---- A8: double instantiation ----
    {
        std::printf("== A8 double instantiation\n");
        using MD = CSRMatrix<double>;
        std::mt19937 rng(13);
        std::vector<std::tuple<idx_t, idx_t, float>> ta, tb;
        std::uniform_int_distribution<idx_t> ad(0, 59);
        for (idx_t r = 0; r < 40; ++r)
            for (int t = 0; t < 8; ++t) ta.emplace_back(r, ad(rng), 1.0f);
        for (idx_t r = 0; r < 60; ++r)
            for (int t = 0; t < 8; ++t) tb.emplace_back(r, ad(rng), 1.5f);
        // Build double matrices through the same Row-Split API.
        auto mkd = [](idx_t rows, idx_t cols,
                      std::vector<std::tuple<idx_t, idx_t, float>>& in) {
            MD m(rows, cols);
            int nt = omp_get_max_threads();
            std::vector<MD::PaddedAllocator> allocs;
            for (int i = 0; i < nt; ++i) allocs.emplace_back();
            std::vector<std::pair<idx_t, idx_t>> ranges(nt);
            size_t chunk = (rows + nt - 1) / nt;
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                auto& al = *allocs[tid].ptr;
                idx_t rs = (idx_t)std::min<size_t>(rows, tid * chunk);
                idx_t re = (idx_t)std::min<size_t>(rows, (tid + 1) * chunk);
                ranges[tid] = {rs, re};
                for (auto& t : in) {
                    idx_t r = std::get<0>(t);
                    if (r >= rs && r < re) {
                        MD::Triplet* tr = al.allocate();
                        tr->row = r; tr->col = std::get<1>(t);
                        tr->value = (double)std::get<2>(t); tr->is_set = false;
                    }
                }
            }
            MD::build_from_partial_results(m, allocs, ranges);
            return m;
        };
        MD A = mkd(40, 60, ta);
        MD B = mkd(60, 500000, tb);  // hash branch
        MD got = MD::multiply_and_prune(A, B, 6);
        CHECK(got.nnz() == 40 * 6, "A8: limit entries per row");
        CHECK(got.rows() == 40 && got.cols() == 500000, "A8: dims");
    }

    // ---- A9: thread determinism is driven externally via OMP_NUM_THREADS
    //         (the driver runs this binary at 1, 2, 4, 8 threads and compares
    //         output hashes) -- nothing to do in-process.

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(s) FAILED\n", g_failures);
    return 1;
}


