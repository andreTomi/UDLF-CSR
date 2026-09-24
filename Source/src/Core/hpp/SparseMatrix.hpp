#ifndef SPARSE_MATRIX_HPP
#define SPARSE_MATRIX_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <numeric>
#include <iostream>
#include <cmath>
#include <omp.h>

#include "BlockAllocator.hpp" 


template <typename T>
class CSRMatrix {
public:
    using value_type = T;
    using size_type = uint32_t;
    using index_type = uint32_t;

    struct Triplet {
        size_type row;
        size_type col;
        T value;
        bool is_set; 
        
        Triplet() : row(0), col(0), value(T{}), is_set(false) {}
        Triplet(size_type r, size_type c, T v, bool set = false)
            : row(r), col(c), value(v), is_set(set) {}
    };

    // CRITICAL FIX: Heap-allocated padded structure to prevent false sharing
    struct PaddedAllocator {
        alignas(128) BlockAllocator<Triplet>* ptr;  // 128-byte alignment for safety
        char padding[128 - sizeof(BlockAllocator<Triplet>*)];  // Explicit padding
        
        PaddedAllocator() : ptr(nullptr) {
            // Allocate on heap with alignment
            ptr = new (std::align_val_t(128)) BlockAllocator<Triplet>();
        }
        
        ~PaddedAllocator() {
            if (ptr) {
                ptr->~BlockAllocator();
                ::operator delete(ptr, std::align_val_t(128));
            }
        }
        
        // Delete copy/move
        PaddedAllocator(const PaddedAllocator&) = delete;
        PaddedAllocator& operator=(const PaddedAllocator&) = delete;
        PaddedAllocator(PaddedAllocator&& other) noexcept : ptr(other.ptr) {
            other.ptr = nullptr;
        }
        PaddedAllocator& operator=(PaddedAllocator&& other) noexcept {
            if (this != &other) {
                if (ptr) {
                    ptr->~BlockAllocator();
                    ::operator delete(ptr, std::align_val_t(128));
                }
                ptr = other.ptr;
                other.ptr = nullptr;
            }
            return *this;
        }
    };

    CSRMatrix() : rows_(0), cols_(0), finalized_(false) {}
    CSRMatrix(size_type rows, size_type cols, size_t estimated_nnz = 0)
        : rows_(rows), cols_(cols), finalized_(false) {
        row_ptr_.resize(rows + 1, 0);
        if (estimated_nnz > 0) {
            values_.reserve(estimated_nnz);
            col_idx_.reserve(estimated_nnz);
        }
    }

    CSRMatrix(CSRMatrix&& other) noexcept = default;
    CSRMatrix& operator=(CSRMatrix&& other) noexcept = default;

    // =========================================================================
    // OPTIMIZED "ROW-SPLIT" ENGINE
    // =========================================================================

    // =========================================================================
    // Builder v2 -- the SINGLE builder (S2.3 step 2 consolidation).
    //
    // It replaces both alternatives removed in that step: the legacy
    // global-sort builder (temp_triplets_ + std::sort(par_unseq), which S2.1
    // had kept as the A/B reference) and the fragment-CSR builder
    // (build_from_ordered_fragments), whose emission-order invariant the
    // callers could not guarantee -- multiply_and_prune's LHRR symmetric case
    // emits rows outside the emitting thread's range. Every producer in the
    // engine (multiply_and_prune, merge, combine, the methods) routes here.
    //
    // CONTRACT (what any caller -- present or future method -- may rely on):
    //   * Emission order is FREE: any thread, any row, any column order, and
    //     `row` may fall outside the emitting thread's row range (the LHRR
    //     symmetric mainMatrix case). The producer needs no invariants.
    //   * Output: row_ptr_ monotone from 0, columns strictly increasing inside
    //     each row (get() is a lower_bound, merge() is a two-pointer).
    //   * Duplicate (row,col): is_set == false sums, is_set == true replaces,
    //     processed in record order (thread id ascending, then emission order).
    //   * Memory: NO copy of the emitted stream. v2 needs NT*rows*size_type B
    //     of cursors plus the CSR output itself (the retired legacy builder
    //     first materialized the whole stream: 16 B per record -- 921.6 MB on
    //     the ALOI position build).
    //   * Determinism: bit-identical for a fixed thread count. Across thread
    //     counts only the summation order of a cell with >= 3 duplicate
    //     records is unspecified (last bits of the sum).
    //
    // V4 fast path (S2.4/Fase 1): the per-row sort is skipped when the row
    // already reaches V4 strictly increasing by column -- see
    // v2_row_is_presorted() for why the criterion is strict and how the skip
    // stays bit-identical to sorting. No clause above changes.
    //
    // Phases: V1 histogram (P_B3_HIST) -> V2 prefix + per-thread cursors
    // (P_B3_PREFIX) -> V3 scatter (P_B5_FILL) -> V4 per-row sort + coalesce
    // (P_V4_ROWSORT) -> V5 compaction (P_V5_COMPACT, skipped when nothing
    // shrank).
    //
    // V5 is SERIAL BY DESIGN. With new_ptr[r] <= old_ptr[r] (prefix of counts
    // that only shrink), the destination range of row r can overlap the SOURCE
    // range of earlier rows, so a parallel per-row memmove is a data race;
    // ascending serial order is safe because every earlier row has already been
    // read and written when row r moves, and new_ptr[r+1] <= old_ptr[r+1] means
    // the move never touches a row that is still pending. Known debt: this is a
    // new serial step, O(nnz); re-evaluate if a future dataset pushes the
    // dedupe fraction or nnz up by orders of magnitude.
    // =========================================================================

    // Row-sort strategy inside v2 (measured in the S2 A/B):
    //   0 = index permutation + contiguous pair scratch (default)
    //   1 = zip iterator: a single std::sort permutes both arrays in lockstep
    inline static int v2_row_sort_mode_ = 0;

public:
    static void set_v2_row_sort_mode(int m) { v2_row_sort_mode_ = (m == 1) ? 1 : 0; }
    static int v2_row_sort_mode() { return v2_row_sort_mode_; }

    // V4 fast-path predicate (S2.4/Fase 1): build_from_partial_results skips
    // the per-row sort iff this returns true. STRICT on purpose -- any
    // adjacent duplicate (cols[i-1] == cols[i]) yields false, so rows that
    // need coalescing always take the full sort. std::sort is unstable: for
    // unique columns the sorted array is unique, hence skip and sort produce
    // the same array BY PROOF, not by convention; with duplicates, skipping
    // could permute equal keys relative to the pre-fast-path behaviour and
    // change the is_set resolution order and the float summation order
    // inside coalesce (addition is not associative). Replacing the criterion
    // with std::is_sorted (which ACCEPTS duplicates) reopens that gap --
    // guarded by C23 in Source/tests/test_csr_ops.cpp.
    static bool v2_row_is_presorted(const index_type* cols, size_t m) {
        for (size_t i = 1; i < m; ++i)
            if (!(cols[i - 1] < cols[i])) return false;
        return true;
    }

private:
    // Zip random-access iterator over (col_idx_, values_). The reference is a
    // proxy that writes through to both arrays and converts to
    // std::pair<index_type,T> so libstdc++'s introsort can move elements
    // through its internal _ValueType. Used only when mode == 1; covered by
    // the adversarial C22 pairing test.
    struct RowZipRef {
        index_type* c;
        T* v;
        RowZipRef(index_type* cc, T* vv) : c(cc), v(vv) {}
        operator std::pair<index_type, T>() const {
            return std::pair<index_type, T>(*c, *v);
        }
        RowZipRef& operator=(const RowZipRef& o) { *c = *o.c; *v = *o.v; return *this; }
        RowZipRef& operator=(const std::pair<index_type, T>& o) {
            *c = o.first; *v = o.second; return *this;
        }
        // std::iter_swap does an unqualified swap(*a, *b), and operator* yields
        // a prvalue proxy, so std::swap (T&) is not viable. This ADL-visible
        // overload takes the proxies by value and swaps the pointed-to slots,
        // which is what lets libstdc++'s introsort move elements.
        friend void swap(RowZipRef a, RowZipRef b) {
            std::swap(*a.c, *b.c);
            std::swap(*a.v, *b.v);
        }
    };
    struct RowZipIt {
        index_type* c;
        T* v;
        using iterator_category = std::random_access_iterator_tag;
        using value_type = std::pair<index_type, T>;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = RowZipRef;
        RowZipRef operator*() const { return RowZipRef(c, v); }
        RowZipRef operator[](difference_type n) const { return RowZipRef(c + n, v + n); }
        RowZipIt& operator++() { ++c; ++v; return *this; }
        RowZipIt operator++(int) { RowZipIt t = *this; ++c; ++v; return t; }
        RowZipIt& operator--() { --c; --v; return *this; }
        RowZipIt operator--(int) { RowZipIt t = *this; --c; --v; return t; }
        RowZipIt& operator+=(difference_type n) { c += n; v += n; return *this; }
        RowZipIt& operator-=(difference_type n) { c -= n; v -= n; return *this; }
        RowZipIt operator+(difference_type n) const { return RowZipIt{c + n, v + n}; }
        RowZipIt operator-(difference_type n) const { return RowZipIt{c - n, v - n}; }
        difference_type operator-(const RowZipIt& o) const { return c - o.c; }
        bool operator==(const RowZipIt& o) const { return c == o.c; }
        bool operator!=(const RowZipIt& o) const { return c != o.c; }
        bool operator<(const RowZipIt& o) const { return c < o.c; }
    };
    struct RowZipLess {
        // libstdc++'s introsort compares a mix of *it (the proxy) and its
        // internal _ValueType (std::pair) moved out of the range, so the
        // comparator must accept both through the zcol() overload set.
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            return zcol(a) < zcol(b);
        }
    };
    static index_type zcol(const RowZipRef& r) { return *r.c; }
    static index_type zcol(const std::pair<index_type, T>& p) { return p.first; }

public:
    static void build_from_partial_results(
        CSRMatrix& C, 
        std::vector<PaddedAllocator>& allocators,
        const std::vector<std::pair<size_type, size_type>>& row_ranges
    ) {
        if (C.finalized_) throw std::logic_error("Matrix finalized");
        (void)row_ranges;   // scheduling hint only: v2 needs no range invariant
        const int NT = (int)allocators.size();
        const size_t NR = C.rows_;

        // ---- V1: per-thread row histogram + is_set detection ----------------
        std::vector<size_type> cur((size_t)NT * NR, 0);
        std::vector<char> any_set_t((size_t)NT, 0);
        {
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                if (tid < NT && allocators[tid].ptr->size() > 0) {
                    size_type* mine = &cur[(size_t)tid * NR];
                    char s = 0;
                    allocators[tid].ptr->for_each([&](const Triplet& t) {
                        mine[t.row]++;
                        s |= (char)t.is_set;
                    });
                    any_set_t[(size_t)tid] = s;
                }
            }
        }
        bool any_set = false;
        for (int t = 0; t < NT; ++t) any_set |= (any_set_t[(size_t)t] != 0);

        // ---- V2: row_ptr_ prefix + in-place histogram -> cursor transform ---
        // One serial pass: the row totals give row_ptr_, and the same pass
        // rewrites each thread's count into its absolute cursor in that row.
        size_t total = 0;
        for (size_t r = 0; r < NR; ++r) {
            C.row_ptr_[r] = (index_type)total;
            size_t off = total;
            for (int t = 0; t < NT; ++t) {
                size_type c = cur[(size_t)t * NR + r];
                cur[(size_t)t * NR + r] = (size_type)off;
                off += c;
            }
            total = off;
        }
        C.row_ptr_[NR] = (index_type)total;
        std::cout << "  [BuildV2] Total triplets: " << total << std::endl;

        // ---- V3: scatter each thread's own stream into its cursor slots -----
        // Order inside a row = (thread id, emission order): deterministic, and
        // it is deterministic for a fixed thread count.
        C.values_.resize(total);
        C.col_idx_.resize(total);
        std::vector<uint8_t> flags;
        if (any_set) flags.assign(total, 0);
        {
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                if (tid < NT && allocators[tid].ptr->size() > 0) {
                    size_type* mine = &cur[(size_t)tid * NR];
                    uint8_t* fl = any_set ? flags.data() : nullptr;
                    allocators[tid].ptr->for_each([&](const Triplet& t) {
                        size_type pos = mine[t.row]++;
                        C.values_[pos] = t.value;
                        C.col_idx_[pos] = t.col;
                        if (fl) fl[pos] = (uint8_t)(t.is_set ? 1 : 0);
                    });
                }
            }
        }
        // ---- V4: per-row sort by column + coalesce (parallel over rows) -----
        std::vector<size_type> new_cnt(NR, 0);
        size_t kept_total = 0;
        {
            #pragma omp parallel reduction(+:kept_total)
            {
                std::vector<index_type> idx;                    // mode 0 scratch
                std::vector<std::pair<index_type, T>> buf;      // mode 0 scratch
                size_t kept = 0;
                #pragma omp for schedule(dynamic, 256)
                for (long long r = 0; r < (long long)NR; ++r) {
                    const size_t rs = C.row_ptr_[r];
                    const size_t m = (size_t)C.row_ptr_[r + 1] - rs;
                    if (m == 0) continue;
                    index_type* cols = C.col_idx_.data() + rs;
                    T* vals = C.values_.data() + rs;
                    // S2.4/Fase 1 fast path: skip only when the row already
                    // arrives strictly increasing (unique columns => the
                    // sorted array is unique => skip == sort bit-for-bit);
                    // rows with adjacent duplicates fall through to the full
                    // sort, preserving the pre-fast-path behaviour exactly.
                    if (m > 1 && !v2_row_is_presorted(cols, m)) {
                        if (v2_row_sort_mode_ == 1) {
                            std::sort(RowZipIt{cols, vals},
                                      RowZipIt{cols + m, vals + m},
                                      RowZipLess{});
                        } else {
                            idx.resize(m);
                            for (size_t i = 0; i < m; ++i) idx[i] = (index_type)i;
                            std::sort(idx.begin(), idx.end(),
                                      [cols](index_type a, index_type b) {
                                          return cols[a] < cols[b];
                                      });
                            buf.resize(m);
                            for (size_t i = 0; i < m; ++i)
                                buf[i] = std::make_pair(cols[idx[i]], vals[idx[i]]);
                            for (size_t i = 0; i < m; ++i) {
                                cols[i] = buf[i].first;
                                vals[i] = buf[i].second;
                            }
                        }
                    }
                    uint8_t* fl = any_set ? flags.data() + rs : nullptr;
                    size_t w = 0;
                    for (size_t i = 1; i < m; ++i) {
                        if (cols[i] == cols[w]) {
                            if (fl && fl[i]) vals[w] = vals[i];   // is_set: replace
                            else vals[w] += vals[i];              // else: sum
                        } else {
                            ++w;
                            cols[w] = cols[i];
                            vals[w] = vals[i];
                            if (fl) fl[w] = fl[i];
                        }
                    }
                    new_cnt[(size_t)r] = (size_type)(w + 1);
                    kept += w + 1;
                }
                kept_total += kept;
            }
        }

        // ---- V5: compaction across rows (serial; see the contract note) -----
        if (kept_total != total) {
            std::vector<size_type> new_ptr(NR + 1);
            size_t acc = 0;
            for (size_t r = 0; r < NR; ++r) {
                new_ptr[r] = (size_type)acc;
                acc += new_cnt[r];
            }
            new_ptr[NR] = (size_type)acc;
            for (size_t r = 0; r < NR; ++r) {
                const size_t src = C.row_ptr_[r];
                const size_t dst = new_ptr[r];
                const size_t len = new_cnt[r];
                if (len == 0 || dst == src) continue;
                std::memmove(C.values_.data() + dst, C.values_.data() + src,
                             len * sizeof(T));
                std::memmove(C.col_idx_.data() + dst, C.col_idx_.data() + src,
                             len * sizeof(index_type));
            }
            std::memcpy(C.row_ptr_.data(), new_ptr.data(),
                        (NR + 1) * sizeof(index_type));
        }
        // Trim the logical size; the capacity (the pre-dedupe footprint) stays
        // on purpose -- shrink_to_fit() would reallocate and spike the peak,
        // which is the memory this builder exists to remove.
        C.values_.resize(kept_total);
        C.col_idx_.resize(kept_total);
        C.finalized_ = true;

#ifndef NDEBUG
        // Builder-side invariant check (debug builds only): every producer of
        // every future method goes through here, so a regression fails loudly.
        assert(C.row_ptr_[NR] == (index_type)C.values_.size());
        assert(C.row_ptr_[NR] == (index_type)C.col_idx_.size());
        for (size_t r = 0; r < NR; ++r)
            for (index_type i = C.row_ptr_[r]; i + 1 < C.row_ptr_[r + 1]; ++i)
                assert(C.col_idx_[i] < C.col_idx_[i + 1]);
#endif

    }

private:
    // =========================================================================
    // Row accumulators for multiply_and_prune
    //
    // The previous implementation used one O(B.cols_) dense array per thread
    // (`dense_acc`/`marker`). Real touched work per row is bounded by
    // bound_i = sum_{k in row_i(A)} nnz_B(k)  -- i.e. O(k^2), not O(n) --
    // but that O(n) buffer means the k^2 touches are scattered across a
    // huge, mostly-empty address range, which is the actual source of the
    // cache/TLB pressure (not extra arithmetic work: the inner double loop
    // was already bounded by k^2).
    //
    // HashRowAccumulator below caps the accumulator's footprint at
    // O(max_bound) for the thread's row range, so it can stay cache
    // resident regardless of B.cols_. Empirically (see
    // Source/bench/bench_accumulator.cpp) it wins once B.cols_ is roughly
    // ADAPTIVE_RATIO times the bound; below that the extra hashing/probing
    // cost isn't worth it and the plain dense array remains faster, so
    // multiply_and_prune picks per-thread-chunk between the two.
    // =========================================================================

    struct DenseRowAccumulator {
        std::vector<T> val;
        std::vector<index_type> marker;
        std::vector<size_t> active_;
        index_type current_row_ = (index_type)-1;

        explicit DenseRowAccumulator(size_type n) : val(n, T{}), marker(n, (index_type)-1) {}

        void begin_row(index_type row_id) {
            active_.clear();
            current_row_ = row_id;
        }
        inline void accumulate(index_type col, T v) {
            if (marker[col] != current_row_) {
                marker[col] = current_row_;
                val[col] = T{};
                active_.push_back(col);
            }
            val[col] += v;
        }
        std::vector<size_t>& used() { return active_; }
        index_type col_of(size_t slot) const { return (index_type)slot; }
        T val_of(size_t slot) const { return val[slot]; }
    };

    struct HashRowAccumulator {
        static constexpr index_type EMPTY = (index_type)-1;
        std::vector<index_type> slot_col;
        std::vector<T> slot_val;
        std::vector<size_t> used_;
        size_t mask_ = 0;

        static size_t next_pow2(size_t x) {
            size_t p = 1;
            while (p < x) p <<= 1;
            return p;
        }
        // Called once per thread with the max per-row bound in its chunk.
        // Distinct columns touched in a row are <= bound, and capacity is
        // 2*bound (rounded up to a power of two), so load factor stays
        // <= 50% and linear probing cannot degenerate into an infinite loop.
        void reserve_capacity(size_t bound) {
            size_t capacity = next_pow2(std::max<size_t>(4, bound * 2));
            slot_col.assign(capacity, EMPTY);
            slot_val.assign(capacity, T{});
            mask_ = capacity - 1;
        }
        void begin_row(index_type /*row_id*/) {
            for (size_t s : used_) slot_col[s] = EMPTY;  // O(k), not O(capacity)
            used_.clear();
        }
        static inline size_t mix(index_type x) {
            uint64_t h = x;
            h ^= h >> 16; h *= 0x85ebca6bULL;
            h ^= h >> 13; h *= 0xc2b2ae35ULL;
            h ^= h >> 16;
            return (size_t)h;
        }
        inline void accumulate(index_type col, T v) {
            size_t h = mix(col) & mask_;
            while (slot_col[h] != EMPTY && slot_col[h] != col) h = (h + 1) & mask_;
            if (slot_col[h] == EMPTY) {
                // The table only grows on this branch (existing-slot updates
                // never touch used_), so guarding the load-factor invariant here
                // costs O(k) per row instead of one branch per accumulate call.
                assert(used_.size() < (mask_ + 1) / 2);  // insert cannot saturate
                slot_col[h] = col;
                slot_val[h] = v;
                used_.push_back(h);
            } else {
                slot_val[h] += v;
            }
        }
        std::vector<size_t>& used() { return used_; }
        index_type col_of(size_t slot) const { return slot_col[slot]; }
        T val_of(size_t slot) const { return slot_val[slot]; }
    };

    // Shared body: fan out A-row i through B, prune to `limit`, emit Triplets.
    // Identical for both accumulator kinds via the used()/col_of()/val_of() interface.
    //
    // IMPORTANT: `used` is a reference to the accumulator's own bookkeeping
    // list. For HashRowAccumulator, begin_row() clears exactly the slots
    // recorded in that list on the NEXT row, so it must still list every
    // slot actually written this row when we're done here -- truncating it
    // to `limit` would silently leak the pruned-away slots (never marked
    // EMPTY again, never re-tracked), which fills the table with stale
    // entries after a few rows and makes accumulate()'s probe loop spin
    // forever once it's full. nth_element only reorders in place, so we
    // prune by adjusting how many *leading* entries we emit, not by
    // resizing the list itself.
    template <typename Acc>
    static void multiply_prune_row(size_type i, const CSRMatrix& A, const CSRMatrix& B,
                                    size_t limit, Acc& acc, BlockAllocator<Triplet>& allocator
                                    ) {
        for (size_type idx_a = A.row_ptr_[i]; idx_a < A.row_ptr_[i + 1]; ++idx_a) {
            size_type k = A.col_idx_[idx_a];
            T val_a = A.values_[idx_a];
            for (size_type idx_b = B.row_ptr_[k]; idx_b < B.row_ptr_[k + 1]; ++idx_b) {
                acc.accumulate(B.col_idx_[idx_b], val_a * B.values_[idx_b]);
            }
        }

        auto& used = acc.used();
        size_t emit_count = used.size();
        if (emit_count > limit) {
            std::nth_element(used.begin(), used.begin() + limit, used.end(),
                [&](size_t a, size_t b) {
                    return std::abs(acc.val_of(a)) > std::abs(acc.val_of(b));
                });
            emit_count = limit;  // do NOT used.resize(...) -- see note above
        }

        // Sort columns within this row: nth_element reorders by |v|, not by
        // col, and even the un-pruned order is the accumulator's FIRST-TOUCH
        // order, which is not ascending whenever row i of A has more than one
        // k and the k-th rows of B overlap out of column order. v2 tolerates
        // any order, but the emitted order has to stay deterministic (the
        // per-row coalesce sums duplicate records in emission order) and the
        // consumers binary-search, so both branches sort.
        if constexpr (std::is_same_v<Acc, DenseRowAccumulator>) {
            // col_of(slot) == slot: ordering the slots ascending IS ordering
            // the columns. Plain integer sort -- no indirect comparator.
            std::sort(used.begin(), used.begin() + emit_count);
        } else {
            std::sort(used.begin(), used.begin() + emit_count,
                [&](size_t a, size_t b) {
                    return acc.col_of(a) < acc.col_of(b);
                });
        }
#ifndef NDEBUG
        // Emitted-order invariant (debug builds only): every consumer
        // binary-searches the row, so this must hold.
        for (size_t idx = 1; idx < emit_count; ++idx)
            assert(acc.col_of(used[idx - 1]) < acc.col_of(used[idx]));
#endif
        for (size_t idx = 0; idx < emit_count; ++idx) {
            size_t slot = used[idx];
            T v = acc.val_of(slot);
            if (std::abs(v) > 1e-9) {
                Triplet* t = allocator.allocate();
                t->row = i; t->col = acc.col_of(slot); t->value = v; t->is_set = false;
            }
        }
    }

public:
    // =========================================================================
    // Matrix Operations (Using optimized build)
    // =========================================================================

    static CSRMatrix multiply_and_prune(const CSRMatrix& A, const CSRMatrix& B, size_t limit) {
        if (A.cols_ != B.rows_) throw std::invalid_argument("Dimension mismatch");

        CSRMatrix C(A.rows_, B.cols_, 0);
        int num_threads = omp_get_max_threads();
        
        // CRITICAL: Separate allocation to prevent false sharing
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

        // Below this B.cols_/bound ratio the plain dense array is faster in
        // practice (hash probing overhead isn't paid back yet). Calibration
        // (single core, ra=rb=20, bound=400; Source/bench/bench_accumulator.cpp):
        //   ratio    25 (N=10k):  dense 0.95x   -> guard must stay dense here
        //   ratio   250 (N=100k): hash  1.16x
        //   ratio  2500 (N=1M):   hash  1.70x
        //   ratio  5000 (N=2M):   hash  2.19x
        //   ratio 125000 (N=50M): hash  3.64x
        // Hash already wins at ratio ~250 on the calibration host, so 200 is
        // a validated conservative default. Re-benchmark on the target
        // hardware / sparsity profile before trusting it blindly.
        constexpr size_t ADAPTIVE_RATIO = 200;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;

            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

            // Real per-row fan-out bound for this thread's rows:
            // bound_i = sum_{k in row_i(A)} nnz_B(k)  ->  O(k^2), not O(n).
            size_t max_bound = 0;
            for (size_t i = r_start; i < r_end; ++i) {
                size_t bound = 0;
                for (size_type idx_a = A.row_ptr_[i]; idx_a < A.row_ptr_[i+1]; ++idx_a) {
                    size_type k = A.col_idx_[idx_a];
                    bound += (B.row_ptr_[k+1] - B.row_ptr_[k]);
                }
                max_bound = std::max(max_bound, bound);
            }

            bool use_hash = (size_t)B.cols_ > ADAPTIVE_RATIO * std::max<size_t>(max_bound, 1);

            if (use_hash) {
                HashRowAccumulator acc;
                acc.reserve_capacity(max_bound);
                for (size_t i = r_start; i < r_end; ++i) {
                    acc.begin_row((index_type)i);
                    multiply_prune_row(i, A, B, limit, acc, allocator);
                }
            } else {
                DenseRowAccumulator acc(B.cols_);
                for (size_t i = r_start; i < r_end; ++i) {
                    acc.begin_row((index_type)i);
                    multiply_prune_row(i, A, B, limit, acc, allocator);
                }
            }
        }
        
        build_from_partial_results(C, allocators, ranges);
        return C;
    }

    template <typename BinaryOp>
    static CSRMatrix merge(const CSRMatrix& A, const CSRMatrix& B, BinaryOp op) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;
            
            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

            for (size_t i = r_start; i < r_end; ++i) {
                size_t idx_a = A.row_ptr_[i], end_a = A.row_ptr_[i+1];
                size_t idx_b = B.row_ptr_[i], end_b = B.row_ptr_[i+1];

                while (idx_a < end_a && idx_b < end_b) {
                    size_t ca = A.col_idx_[idx_a];
                    size_t cb = B.col_idx_[idx_b];
                    if (ca < cb) {
                        T res = op(A.values_[idx_a], T{});
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = ca; t->value = res; t->is_set = false;
                        }
                        idx_a++;
                    } else if (cb < ca) {
                        T res = op(T{}, B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = cb; t->value = res; t->is_set = false;
                        }
                        idx_b++;
                    } else {
                        T res = op(A.values_[idx_a], B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = ca; t->value = res; t->is_set = false;
                        }
                        idx_a++; idx_b++;
                    }
                }
                
                while (idx_a < end_a) {
                    T res = op(A.values_[idx_a], T{});
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = A.col_idx_[idx_a]; t->value = res; t->is_set = false;
                    }
                    idx_a++;
                }
                
                while (idx_b < end_b) {
                    T res = op(T{}, B.values_[idx_b]);
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = B.col_idx_[idx_b]; t->value = res; t->is_set = false;
                    }
                    idx_b++;
                }
            }
        }
        
        build_from_partial_results(C, allocators, ranges);
        return C;
    }

    static CSRMatrix combine(const CSRMatrix& A, const CSRMatrix& B) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;
            
            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

            for (size_t i = r_start; i < r_end; ++i) {
                size_t nnz_a = A.row_ptr_[i+1] - A.row_ptr_[i];
                size_t nnz_b = B.row_ptr_[i+1] - B.row_ptr_[i];
                if (nnz_a == 0 || nnz_b == 0) continue;

                if (nnz_a < nnz_b) {
                    for (size_type idx = A.row_ptr_[i]; idx < A.row_ptr_[i+1]; ++idx) {
                        size_type col = A.col_idx_[idx];
                        T val_b = B.get(i, col);
                        if (val_b != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = col; t->value = A.values_[idx] * val_b; t->is_set = false;
                        }
                    }
                } else {
                    for (size_type idx = B.row_ptr_[i]; idx < B.row_ptr_[i+1]; ++idx) {
                        size_type col = B.col_idx_[idx];
                        T val_a = A.get(i, col);
                        if (val_a != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = col; t->value = val_a * B.values_[idx]; t->is_set = false;
                        }
                    }
                }
            }
        }
        
        build_from_partial_results(C, allocators, ranges);
        return C;
    }

    // =========================================================================
    // Utility Functions
    // =========================================================================
    
    size_t nnz() const { return values_.size(); }
    size_type rows() const { return rows_; }
    size_type cols() const { return cols_; }
    size_type row_nnz(size_type row) const { 
        return row_ptr_[row+1] - row_ptr_[row]; 
    }
    
    T get(size_type row, size_type col) const {
        if (row >= rows_) return T{};
        auto it = std::lower_bound(col_idx_.begin() + row_ptr_[row], 
                                   col_idx_.begin() + row_ptr_[row+1], col);
        if (it != col_idx_.begin() + row_ptr_[row+1] && *it == col) {
            return values_[std::distance(col_idx_.begin(), it)];
        }
        return T{};
    }

    template <typename Func>
    void for_each_in_row(size_type row, Func&& func) const {
        for (size_type i = row_ptr_[row]; i < row_ptr_[row+1]; ++i) {
            func(col_idx_[i], values_[i]);
        }
    }

    CSRMatrix transpose() const {
        CSRMatrix result(cols_, rows_, values_.size());
        std::vector<size_type> counts(cols_, 0);
        for (auto c : col_idx_) counts[c]++;
        
        result.row_ptr_[0] = 0;
        for (size_type i = 0; i < cols_; ++i) {
            result.row_ptr_[i+1] = result.row_ptr_[i] + counts[i];
        }
        
        result.values_.resize(values_.size());
        result.col_idx_.resize(col_idx_.size());
        
        std::vector<size_type> current(cols_);
        for (size_type i = 0; i < cols_; ++i) current[i] = result.row_ptr_[i];
        
        for (size_type r = 0; r < rows_; ++r) {
            for (size_type j = row_ptr_[r]; j < row_ptr_[r+1]; ++j) {
                size_type c = col_idx_[j];
                size_type dest = current[c]++;
                result.values_[dest] = values_[j];
                result.col_idx_[dest] = r;
            }
        }
        
        result.finalized_ = true;
        return result;
    }
    
    void reset() {
        finalized_ = false;
        values_.clear();
        col_idx_.clear();
        std::fill(row_ptr_.begin(), row_ptr_.end(), 0);
    }

private:
    size_type rows_, cols_;
    bool finalized_;
    std::vector<T> values_;
    std::vector<index_type> col_idx_, row_ptr_;
};

#endif // SPARSE_MATRIX_HPP
