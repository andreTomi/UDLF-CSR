// test_csr_ops.cpp
//
// Characterization suite for the CSR helpers that Fase 1 (fragment-CSR) will
// rewrite the producers of: build_from_partial_results, merge, combine and
// transpose (Source/src/Core/hpp/SparseMatrix.hpp).
//
// "Characterization" means: every assertion below is the behaviour the code
// has TODAY, verified by running it -- including the asymmetric ones that are
// documented as such instead of being silently frozen. This file exists so
// that the build rewrite can be judged by behaviour, not by inspection.
//
// Build (WSL, from repo root) -- same optimisation flags as the release build
// (-O2), because the kernel is compiled with -ffast-math -march=native and the
// tests must not diverge from it:
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src \
//       Source/tests/test_csr_ops.cpp -o /tmp/test_csr_ops -ltbb
//
// FP policy: -ffast-math lets the release kernel reassociate and fuse, so a
// suite that asserted sums could drift from the kernel. This suite only
// asserts operations that are EXACT in binary32 regardless of contraction or
// reassociation -- copies, +/- on values from {0.25, 0.5, 1, 1.5, 2, 2.5, 3,
// 4, 5} and products whose results (12, 35) are exactly representable -- so no
// tolerance and no -ffp-contract=off is needed. If a future case needs
// approximate comparison, add it as a separate helper with an explicit epsilon
// instead of loosening these CHECKs.
//
// Exit code 0 = all tests passed.

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <tuple>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);              \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)
// NOTE: documents behaviour that is deliberate but asymmetric/surprising, so
// it shows up in the log without failing the suite.
#define NOTE(msg) std::printf("  [note] %s\n", msg)

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

// Row-Split producer: every triplet carries an explicit is_set flag.
static M build_csr_flags(idx_t rows, idx_t cols,
                         const std::vector<std::tuple<idx_t, idx_t, float, bool>>& in) {
    M m(rows, cols);
    int nt = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(nt);
    for (int i = 0; i < nt; ++i) allocators.emplace_back();

    std::vector<std::pair<idx_t, idx_t>> ranges(nt);
    size_t chunk = (rows + nt - 1) / nt;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        idx_t r_start = (idx_t)std::min<size_t>(rows, (size_t)tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(rows, (size_t)(tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
        for (const auto& t : in) {
            idx_t r = std::get<0>(t);
            if (r >= r_start && r < r_end) {
                Triplet* tr = allocator.allocate();
                tr->row = r; tr->col = std::get<1>(t);
                tr->value = std::get<2>(t); tr->is_set = std::get<3>(t);
            }
        }
    }
    M::build_from_partial_results(m, allocators, ranges);
    return m;
}

static M build_csr(idx_t rows, idx_t cols,
                   const std::vector<std::tuple<idx_t, idx_t, float>>& in) {
    std::vector<std::tuple<idx_t, idx_t, float, bool>> v;
    v.reserve(in.size());
    for (const auto& t : in)
        v.emplace_back(std::get<0>(t), std::get<1>(t), std::get<2>(t), false);
    return build_csr_flags(rows, cols, v);
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

// (row,col) -> value, following the public row iteration API.
static std::map<std::pair<idx_t, idx_t>, float> to_map(const M& m) {
    std::map<std::pair<idx_t, idx_t>, float> out;
    for (idx_t r = 0; r < m.rows(); ++r)
        m.for_each_in_row(r, [&](idx_t c, float v) { out[{r, c}] = v; });
    return out;
}

// CSR invariants that must hold for ANY matrix the library produces:
//   row_ptr_ monotone from 0, last == nnz, and strictly increasing columns
//   inside each row (get() relies on lower_bound).
static bool csr_well_formed(const M& m, const char** why) {
    if (m.row_nnz(0) != 0 && false) { /* placeholder */ }
    size_t total = 0;
    for (idx_t r = 0; r < m.rows(); ++r) {
        idx_t nnz_r = m.row_nnz(r);
        total += (size_t)nnz_r;
        idx_t prev = (idx_t)-1;
        bool first = true;
        bool ok = true;
        m.for_each_in_row(r, [&](idx_t c, float) {
            if (!first && c <= prev) ok = false;
            prev = c; first = false;
        });
        if (!ok) { *why = "columns not strictly increasing in a row"; return false; }
    }
    if (total != m.nnz()) { *why = "row_ptr_ total != nnz()"; return false; }
    return true;
}

static bool same_structure_values(const M& a, const M& b) {
    if (a.rows() != b.rows() || a.nnz() != b.nnz()) return false;
    return to_map(a) == to_map(b);
}


int main() {
    std::printf("Test suite: CSR ops characterization (threads=%d)\n",
                omp_get_max_threads());
    const char* why = "";

    // ---- C1: builder, no duplicates -- structure and values ----
    {
        std::printf("== C1 builder: structure/value contract\n");
        std::vector<std::tuple<idx_t, idx_t, float>> in = {
            {0, 3, 1.5f}, {0, 0, 2.5f}, {1, 2, -1.0f}, {2, 1, 4.0f}, {2, 0, 0.5f}};
        M A = build_csr(4, 5, in);          // row 3 empty, col 4 unused
        CHECK(A.rows() == 4 && A.cols() == 5, "C1: shape preserved");
        CHECK(A.nnz() == 5, "C1: nnz == number of distinct positions");
        CHECK(A.row_nnz(3) == 0, "C1: empty row stays empty");
        CHECK(csr_well_formed(A, &why), "C1: CSR invariants hold");
        auto g = to_map(A);
        CHECK((g.size() == 5 && g[{0, 0}] == 2.5f && g[{0, 3}] == 1.5f &&
               g[{1, 2}] == -1.0f && g[{2, 0}] == 0.5f && g[{2, 1}] == 4.0f),
              "C1: values land at the right (row,col)");
        std::vector<idx_t> cols_row0;
        A.for_each_in_row(0, [&](idx_t c, float) { cols_row0.push_back(c); });
        CHECK(cols_row0.size() == 2 && cols_row0[0] == 0 && cols_row0[1] == 3,
              "C1: columns sorted even when emitted out of order");
    }

    // ---- C2: builder, duplicate (row,col) with is_set = false ----
    {
        std::printf("== C2 builder duplicates: is_set=false sums\n");
        std::vector<std::tuple<idx_t, idx_t, float, bool>> in = {
            {0, 1, 2.0f, false}, {0, 1, 5.0f, false}, {0, 1, 1.0f, false}};
        M A = build_csr_flags(1, 4, in);
        CHECK(A.nnz() == 1, "C2: three duplicates collapse to one entry");
        CHECK(A.get(0, 1) == 8.0f, "C2: duplicates summed (2+5+1)");
        CHECK(csr_well_formed(A, &why), "C2: CSR invariants hold");
    }

    // ---- C3: builder, duplicate with is_set = true on a LATER triplet ----
    {
        std::printf("== C3 builder duplicates: later is_set=true replaces\n");
        std::vector<std::tuple<idx_t, idx_t, float, bool>> in = {
            {0, 2, 2.0f, false}, {0, 2, 5.0f, true}};
        M A = build_csr_flags(1, 4, in);
        CHECK(A.nnz() == 1, "C3: still one entry");
        CHECK(A.get(0, 2) == 5.0f, "C3: is_set replaced the accumulated 2.0");
    }

    // ---- C4: builder, is_set = true on the FIRST triplet (asymmetric) ----
    {
        std::printf("== C4 builder duplicates: first is_set=true is IGNORED\n");
        std::vector<std::tuple<idx_t, idx_t, float, bool>> in = {
            {0, 3, 5.0f, true}, {0, 3, 2.0f, false}};
        M A = build_csr_flags(1, 4, in);
        CHECK(A.get(0, 3) == 7.0f, "C4: first-taken is_set does not replace");
        NOTE("C4 characterization: only a LATER is_set triplet replaces; a "
             "leading is_set is summed away. Producers must emit their "
             "authoritative value last if they want set semantics.");
    }

    // ---- C5: builder, duplicates straddling the histogram element chunks ----
    // The histogram dedups by comparing element i with i-1 across the whole
    // (sorted) array, so a run spanning a chunk boundary is still counted once.
    // If that ever stops holding, rows past the boundary shift by one slot and
    // every value after it is misaligned -- so assert alignment on the whole
    // matrix, not just on the touched rows.
    {
        std::printf("== C5 builder duplicates across element chunks\n");
        std::vector<std::tuple<idx_t, idx_t, float, bool>> in;
        idx_t rows = 4000;
        for (idx_t r = 0; r < rows; ++r) {
            in.emplace_back(r, 1, 1.0f, false);
            in.emplace_back(r, 1, 1.0f, false);   // pair
            in.emplace_back(r, 1, 1.0f, false);   // triple
            in.emplace_back(r, 7, (float)(r % 3), false);
        }
        M A = build_csr_flags(rows, 16, in);
        CHECK(A.nnz() == (size_t)rows * 2, "C5: one entry per (row,col) group");
        CHECK(csr_well_formed(A, &why), "C5: CSR invariants hold across chunks");
        int bad = 0;
        for (idx_t r = 0; r < rows; ++r) {
            if (A.get(r, 1) != 3.0f) ++bad;
            if (A.get(r, 7) != (float)(r % 3)) ++bad;
        }
        CHECK(bad == 0, "C5: every row reads back its own values (no shift)");

    // ---- C6: merge -- union, op applied with T{} on the missing side ----
    {
        std::printf("== C6 merge: union semantics\n");
        M A = build_csr(2, 5, {{0, 0, 1.0f}, {0, 2, 3.0f}, {1, 1, 2.0f}});
        M B = build_csr(2, 5, {{0, 1, 4.0f}, {1, 3, 5.0f}});
        M C = M::merge(A, B, [](float x, float y) { return x + y; });
        CHECK(C.rows() == 2 && C.cols() == 5, "C6: shape preserved");
        CHECK(C.nnz() == 5, "C6: disjoint columns are unioned");
        auto g = to_map(C);
        CHECK((g[{0, 0}] == 1.0f && g[{0, 1}] == 4.0f && g[{0, 2}] == 3.0f &&
               g[{1, 1}] == 2.0f && g[{1, 3}] == 5.0f),
              "C6: missing side contributes T{} (identity for op)");
        CHECK(csr_well_formed(C, &why), "C6: CSR invariants hold");
    }

    // ---- C7: merge -- overlap applies op to both values ----
    {
        std::printf("== C7 merge: overlap\n");
        M A = build_csr(1, 3, {{0, 1, 2.0f}});
        M B = build_csr(1, 3, {{0, 1, 3.0f}});
        M C = M::merge(A, B, [](float x, float y) { return x + y; });
        CHECK(C.nnz() == 1 && C.get(0, 1) == 5.0f, "C7: overlapping col sums");
    }

    // ---- C8: merge -- a result equal to T{} IS dropped ----
    {
        std::printf("== C8 merge: op result == T{} is dropped\n");
        M A = build_csr(1, 3, {{0, 0, 5.0f}, {0, 2, 1.0f}});
        M B = build_csr(1, 3, {{0, 0, 5.0f}});
        M C = M::merge(A, B, [](float x, float y) { return x - y; });
        CHECK(C.nnz() == 1, "C8: the exact-zero result emits NO slot");
        CHECK(C.get(0, 0) == 0.0f, "C8: cancelled column reads back as absent/0");
        CHECK(C.get(0, 2) == 1.0f, "C8: the surviving column is untouched");
        CHECK(csr_well_formed(C, &why), "C8: CSR invariants hold");
        NOTE("C8: merge drops any op result equal to T{} (5-5 == 0 -> no slot), "
             "so with plus the operator is lossless and with subtraction it is "
             "not: C8 must stay in sync with the fragment-CSR builder.");
    }

    // ---- C9: merge -- row-count mismatch throws, col-count mismatch does not ----
    {
        std::printf("== C9 merge: dimension checks\n");
        M A = build_csr(2, 3, {{0, 0, 1.0f}});
        M B = build_csr(3, 3, {{0, 0, 1.0f}});
        bool threw = false;
        try { M C = M::merge(A, B, [](float x, float y) { return x + y; }); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw, "C9: rows mismatch throws std::invalid_argument");

        M C2 = build_csr(2, 4, {{0, 3, 9.0f}});
        M C3 = M::merge(A, C2, [](float x, float y) { return x + y; });
        CHECK(C3.cols() == 3, "C9: cols are NOT validated (result takes A.cols)");
        NOTE("C9 characterization: only rows_ is checked; a B with extra "
             "columns silently contributes entries with col >= cols().");
    }

    // ---- C10: combine -- Hadamard on the intersection only ----
    {
        std::printf("== C10 combine: intersection semantics\n");
        M A = build_csr(2, 4, {{0, 0, 2.0f}, {0, 1, 3.0f}, {0, 3, 5.0f},
                               {1, 0, 7.0f}});
        M B = build_csr(2, 4, {{0, 1, 4.0f}, {0, 2, 6.0f}, {0, 3, 7.0f},
                               {1, 1, 2.0f}});
        M C = M::combine(A, B);
        CHECK(C.nnz() == 2, "C10: only the shared columns survive");
        CHECK(C.get(0, 1) == 12.0f, "C10: A(0,1)*B(0,1) == 3*4");
        CHECK(C.get(0, 3) == 35.0f, "C10: A(0,3)*B(0,3) == 5*7");
        CHECK(C.get(0, 0) == 0.0f && C.get(1, 0) == 0.0f,
              "C10: A-only / B-only columns are absent");
        CHECK(csr_well_formed(C, &why), "C10: CSR invariants hold");
    }

    // ---- C11: combine -- the zero filter is one-sided ----
    {
        std::printf("== C11 combine: zero filter is one-sided\n");
        // Iteration picks the shorter row, then filters on the value fetched
        // from the OTHER matrix -- so a 0 on the iterated side can still emit.
        M A = build_csr(1, 3, {{0, 1, 2.0f}});
        M B = build_csr(1, 3, {{0, 1, 0.0f}});   // explicit zero entry
        M C = M::combine(A, B);
        NOTE("C11 characterization: combine filters `val_other != T{}`, i.e. it "
             "tests the row it is NOT iterating, so an explicit 0 on the "
             "iterated side still contributes a 0-valued entry.");
        CHECK(C.nnz() == 1 && C.get(0, 1) == 0.0f,
              "C11: 2*0 emitted as an explicit zero entry");
        M A2 = build_csr(1, 3, {{0, 1, 0.0f}});
        M B2 = build_csr(1, 3, {{0, 1, 2.0f}});
        M C2 = M::combine(A2, B2);
        CHECK(C2.nnz() == 0, "C11: mirrored case is filtered (0 on A side)");
    }

    // ---- C12: combine -- row-count mismatch throws ----
    {
        std::printf("== C12 combine: dimension check\n");
        M A = build_csr(2, 3, {{0, 0, 1.0f}});

    // ---- C13: transpose -- rectangular, empty rows, explicit zeros ----
    {
        std::printf("== C13 transpose: rectangular + empty rows\n");
        M A = build_csr(3, 2, {{0, 0, 1.0f}, {0, 1, 2.0f}, {2, 1, 3.0f}});
        M T = A.transpose();
        CHECK(T.rows() == 2 && T.cols() == 3, "C13: shape is swapped");
        CHECK(T.nnz() == A.nnz(), "C13: nnz preserved");
        CHECK(csr_well_formed(T, &why), "C13: CSR invariants hold");
        auto g = to_map(T);
        // A col 0 == {row0:1}   -> T row 0 == {(0,1)}
        // A col 1 == {row0:2, row2:3} -> T row 1 == {(0,2),(2,3)}
        CHECK((g.size() == 3 && g[{0, 0}] == 1.0f && g[{1, 0}] == 2.0f &&
               g[{1, 2}] == 3.0f), "C13: values move to (col,row)");
        CHECK(T.row_nnz(1) == 2, "C13: T row 1 == A col 1 (two entries)");
        CHECK(T.get(1, 1) == 0.0f, "C13: A's empty row 1 has no image");
    }

    // ---- C14: transpose -- an explicit zero entry survives ----
    {
        std::printf("== C14 transpose: explicit zeros survive\n");
        std::vector<std::tuple<idx_t, idx_t, float, bool>> in = {
            {0, 2, 0.0f, false}, {1, 0, 4.0f, false}};
        M A = build_csr_flags(2, 3, in);
        CHECK(A.nnz() == 2, "C14: the builder does not drop explicit zeros");
        M T = A.transpose();
        CHECK(T.nnz() == 2, "C14: transpose copies slots verbatim");
        CHECK(T.get(2, 0) == 0.0f && T.get(0, 1) == 4.0f,
              "C14: zero-valued slot lands at (2,0)");
    }

    // ---- C15: transpose -- involution, bit-exact for float ----
    {
        std::printf("== C15 transpose: double transpose == identity\n");
        std::mt19937 rng(31);
        std::vector<std::tuple<idx_t, idx_t, float>> in;
        for (idx_t r = 0; r < 300; ++r) {
            in.emplace_back(r, (r * 7) % 11, (float)(r % 5) - 2.0f);
            in.emplace_back(r, (r * 3) % 13, 0.25f * (float)(r % 7));
        }
        in.emplace_back(299, 0, 1.0f);   // force a col-0 hit on the last row
        M A = build_csr(300, 17, in);
        M B = A.transpose().transpose();
        CHECK(B.rows() == A.rows() && B.cols() == A.cols(), "C15: shape restored");
        CHECK(B.nnz() == A.nnz(), "C15: nnz restored");
        CHECK(same_structure_values(A, B), "C15: values bit-exact after A -> A^T -> A");
    }

    // ---- C16: transpose of an empty matrix ----
    {
        std::printf("== C16 transpose: empty matrix\n");
        M A = build_csr(5, 3, {});
        M T = A.transpose();
        CHECK(T.rows() == 3 && T.cols() == 5 && T.nnz() == 0,
              "C16: empty stays empty with swapped shape");
        CHECK(csr_well_formed(T, &why), "C16: CSR invariants hold");
    }

    // ---- C17: cross-check merge/combine/transpose against a std::map model ----
    {
        std::printf("== C17 randomized cross-check vs map model\n");
        std::mt19937 rng(97);
        for (int trial = 0; trial < 5; ++trial) {
            idx_t rows = 40, cols = 25;
            std::vector<std::tuple<idx_t, idx_t, float>> ia, ib;
            std::uniform_int_distribution<idx_t> cd(0, cols - 1);
            std::uniform_real_distribution<float> vd(1.0f, 3.0f);
            for (idx_t r = 0; r < rows; ++r) {
                for (int t = 0; t < 4; ++t)
                    ia.emplace_back(r, cd(rng), vd(rng));
                for (int t = 0; t < 4; ++t)
                    ib.emplace_back(r, cd(rng), vd(rng));
            }
            M A = build_csr(rows, cols, ia);
            M B = build_csr(rows, cols, ib);
            auto ma = to_map(A), mb = to_map(B);

            M C = M::merge(A, B, [](float x, float y) { return x + y; });
            auto mc = to_map(C);
            bool ok_merge = (mc.size() == [&] {
                                 std::map<std::pair<idx_t, idx_t>, float> u = ma;
                                 for (auto& kv : mb) u[kv.first] += kv.second;
                                 for (auto it = u.begin(); it != u.end();)
                                     it = (it->second == 0.0f) ? u.erase(it) : ++it;
                                 return u.size();
                             }());
            for (auto& kv : ma) {
                float expect = kv.second + (mb.count(kv.first) ? mb[kv.first] : 0.0f);
                if (mc.count(kv.first) == 0 && expect != 0.0f) ok_merge = false;
                if (mc.count(kv.first) && mc[kv.first] != expect) ok_merge = false;
            }
            CHECK(ok_merge, "C17: merge matches the map model");
            CHECK(csr_well_formed(C, &why), "C17: merge output well formed");

            M D = M::combine(A, B);
            auto md = to_map(D);
            bool ok_comb = true;
            for (auto& kv : md) {
                if (!ma.count(kv.first) || !mb.count(kv.first)) ok_comb = false;
                else if (kv.second != ma[kv.first] * mb[kv.first]) ok_comb = false;
            }
            for (auto& kv : ma) {
                if (!mb.count(kv.first)) continue;
                if (ma[kv.first] * mb[kv.first] != 0.0f &&
                    md[kv.first] != ma[kv.first] * mb[kv.first]) ok_comb = false;
            }
            CHECK(ok_comb, "C17: combine matches the map model");
            CHECK(csr_well_formed(D, &why), "C17: combine output well formed");

            M E = A.transpose();
            bool ok_tr = (E.rows() == A.cols() && E.cols() == A.rows());
            E.for_each_in_row(0, [&](idx_t, float) {});
            for (idx_t r = 0; r < A.rows() && ok_tr; ++r)
                A.for_each_in_row(r, [&](idx_t c, float v) {
                    if (E.get(c, r) != v) ok_tr = false;
                });
            CHECK(ok_tr, "C17: transpose matches the map model");
        }
    }

    // ---- C18: multiply_and_prune -- the DENSE branch must emit columns
    // ascending. Regression guard for the fragment-CSR rewrite (Fase 1): the
    // emitted prefix used to be sorted only for the Hash accumulator, on the
    // argument that col_of(slot) == slot implies ascending order for Dense.
    // It does not: active_ is in FIRST-TOUCH order (accumulate() pushes a
    // column the first time it is touched in the row), and a row of A with
    // two k's whose B rows overlap in non-monotone column order emits columns
    // out of order. get() is lower_bound and merge() is a two-pointer, so an
    // unsorted row silently corrupts every consumer that binary-searches.
    {
        std::printf("== C18 multiply dense branch: emitted columns ascending\n");
        // A row 0 has two k's; B rows overlap with non-monotone columns:
        //   k=0 -> cols {1,3,5}   (B row 0)
        //   k=1 -> cols {0,2,3}   (B row 1)
        // First-touch order in the dense accumulator: [1,3,5,0,2].
        std::vector<std::tuple<idx_t, idx_t, float>> ta = {{0, 0, 1.0f}, {0, 1, 1.0f}};
        std::vector<std::tuple<idx_t, idx_t, float>> tb = {
            {0, 1, 1.0f}, {0, 3, 1.0f}, {0, 5, 1.0f},
            {1, 0, 1.0f}, {1, 2, 1.0f}, {1, 3, 1.0f}};
        M A = build_csr(1, 2, ta);
        M B = build_csr(2, 6, tb);
        // Branch guard: B.cols (6) <= 200 * max_bound (3+3) -> DENSE branch.
        CHECK(B.cols() <= 200 * 6, "C18: case is on the dense branch by ratio");
        {   // unpruned: touched (5) <= limit -> no nth_element at all
            M got = M::multiply_and_prune(A, B, 10);
            std::printf("  [order] row0 cols emitted (limit=10):");
            got.for_each_in_row(0, [&](idx_t c, float) {
                std::printf(" %u", (unsigned)c);
            });
            std::printf("\n");
            CHECK(csr_well_formed(got, &why),
                  "C18: unpruned dense emit is column-sorted");
        }
        {   // pruned: touched (5) > limit (3) -> nth_element permutes the prefix
            M got = M::multiply_and_prune(A, B, 3);
            std::printf("  [order] row0 cols emitted (limit=3):");
            got.for_each_in_row(0, [&](idx_t c, float) {
                std::printf(" %u", (unsigned)c);
            });
            std::printf("\n");
            CHECK(csr_well_formed(got, &why),
                  "C18: pruned dense emit is column-sorted");
        }
        {   // bigger pruned case: touched = 25 (two k's of 15 overlapping),
            // limit = 20 -> nth_element scrambles a 20-entry prefix.
            std::vector<std::tuple<idx_t, idx_t, float>> ta2 = {{0, 0, 1.0f},
                                                                {0, 1, 1.0f}};
            std::vector<std::tuple<idx_t, idx_t, float>> tb2;
            for (idx_t c = 10; c < 25; ++c) tb2.emplace_back(0, c, 1.0f);
            for (idx_t c = 0; c < 15; ++c) tb2.emplace_back(1, c, 1.0f);
            M A2 = build_csr(1, 2, ta2);
            M B2 = build_csr(2, 40, tb2);
            M got = M::multiply_and_prune(A2, B2, 20);
            CHECK(got.nnz() == 20, "C18: limit entries emitted");
            CHECK(csr_well_formed(got, &why),
                  "C18: pruned dense emit is column-sorted (25 touched)");
        }
    }

    std::printf("\n%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "ALL TESTS PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

        M B = build_csr(4, 3, {{0, 0, 1.0f}});
        bool threw = false;
        try { M C = M::combine(A, B); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw, "C12: rows mismatch throws std::invalid_argument");
    }

        NOTE("C5: a duplicate run is collapsed exactly once even when it "
             "straddles an element-chunk boundary.");
    }
