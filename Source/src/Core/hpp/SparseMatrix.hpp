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
#include <execution> 

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

    static void build_from_partial_results(
        CSRMatrix& C, 
        std::vector<PaddedAllocator>& allocators,
        const std::vector<std::pair<size_type, size_type>>& row_ranges
    ) {
        if (C.finalized_) throw std::logic_error("Matrix finalized");

        int num_threads = allocators.size();

        // 1. Calculate offsets (Sequential - very fast)
        std::vector<size_t> thread_offsets(num_threads + 1, 0);
        for (int i = 0; i < num_threads; ++i) {
            thread_offsets[i+1] = thread_offsets[i] + allocators[i].ptr->size();
        }
        size_t total_elements = thread_offsets.back();

        std::cout << "  [Build] Total triplets: " << total_elements << std::endl;

        // 2. Allocate destination buffer
        C.temp_triplets_.resize(total_elements);

        // 3. OPTIMIZED: Parallel copy with prefetching
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            
            if (tid < num_threads && allocators[tid].ptr->size() > 0) {
                auto dest = C.temp_triplets_.begin() + thread_offsets[tid];
                
                // Prefetch hint for next thread's data
                if (tid + 1 < num_threads) {
                    __builtin_prefetch(allocators[tid + 1].ptr, 0, 3);
                }
                
                allocators[tid].ptr->copy_to(dest);
            }
        }

        // 4. Parallel sort (Already optimal)
        std::sort(std::execution::par_unseq, 
                  C.temp_triplets_.begin(), C.temp_triplets_.end(),
                  [](const Triplet& a, const Triplet& b) {
                      if (a.row != b.row) return a.row < b.row;
                      return a.col < b.col;
                  });

        // 5. OPTIMIZED CSR Construction with NUMA awareness
        buildCSRParallel(C, total_elements, row_ranges, num_threads);
        
        C.finalized_ = true;
        std::vector<Triplet>().swap(C.temp_triplets_);
    }

private:
    static void buildCSRParallel(CSRMatrix& C, size_t total_elements, 
                                 const std::vector<std::pair<size_type, size_type>>& row_ranges,
                                 int num_threads) {
        
        // A. NUMA-aware histogram (each thread on its own memory)
        std::vector<std::vector<size_type>> thread_row_counts;
        thread_row_counts.resize(num_threads);
        
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            thread_row_counts[tid].resize(C.rows_, 0);
            
            size_t chunk = (total_elements + num_threads - 1) / num_threads;
            size_t start = std::min(total_elements, (size_t)tid * chunk);
            size_t end   = std::min(total_elements, (size_t)(tid + 1) * chunk);
            
            // Prefetch hint
            if (start < end) {
                __builtin_prefetch(&C.temp_triplets_[start], 0, 3);
            }
            
            for (size_t i = start; i < end; ++i) {
                // Skip duplicates
                if (i > 0 && 
                    C.temp_triplets_[i].row == C.temp_triplets_[i-1].row && 
                    C.temp_triplets_[i].col == C.temp_triplets_[i-1].col) {
                    continue;
                }
                thread_row_counts[tid][C.temp_triplets_[i].row]++;
            }
        }

        // B. Parallel reduction
        std::vector<size_type> row_counts(C.rows_, 0);
        
        #pragma omp parallel for
        for (size_t r = 0; r < C.rows_; ++r) {
            size_type sum = 0;
            for (int t = 0; t < num_threads; ++t) {
                sum += thread_row_counts[t][r];
            }
            row_counts[r] = sum;
        }

        // C. Prefix sum (Sequential but fast)
        C.row_ptr_[0] = 0;
        for (size_t i = 0; i < C.rows_; ++i) {
            C.row_ptr_[i+1] = C.row_ptr_[i] + row_counts[i];
        }
        
        size_t final_nnz = C.row_ptr_[C.rows_];
        C.values_.resize(final_nnz);
        C.col_idx_.resize(final_nnz);

        // D. Parallel fill with cache-aware iteration
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            
            if (tid < (int)row_ranges.size()) {
                size_t r_start = row_ranges[tid].first;
                size_t r_end   = row_ranges[tid].second;

                // Binary search for range
                auto it_start = std::lower_bound(
                    C.temp_triplets_.begin(), C.temp_triplets_.end(), r_start,
                    [](const Triplet& t, size_t val) { return t.row < val; }
                );
                
                auto it_end = std::lower_bound(
                    C.temp_triplets_.begin(), C.temp_triplets_.end(), r_end,
                    [](const Triplet& t, size_t val) { return t.row < val; }
                );

                size_t vec_start = std::distance(C.temp_triplets_.begin(), it_start);
                size_t vec_end   = std::distance(C.temp_triplets_.begin(), it_end);
                size_t write_pos = C.row_ptr_[r_start];

                // Prefetch destination
                if (write_pos < final_nnz) {
                    __builtin_prefetch(&C.values_[write_pos], 1, 3);
                    __builtin_prefetch(&C.col_idx_[write_pos], 1, 3);
                }
                
                for (size_t i = vec_start; i < vec_end; ++i) {
                    if (i > vec_start && 
                        C.temp_triplets_[i].row == C.temp_triplets_[i-1].row &&
                        C.temp_triplets_[i].col == C.temp_triplets_[i-1].col) 
                    {
                        // Duplicate: accumulate or set
                        if (C.temp_triplets_[i].is_set) {
                            C.values_[write_pos-1] = C.temp_triplets_[i].value;
                        } else {
                            C.values_[write_pos-1] += C.temp_triplets_[i].value;
                        }
                    } else {
                        // New entry
                        C.values_[write_pos] = C.temp_triplets_[i].value;
                        C.col_idx_[write_pos] = C.temp_triplets_[i].col;
                        write_pos++;
                    }
                }
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

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;

            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

            // Thread-local accumulators
            std::vector<T> dense_acc(B.cols_, T{});
            std::vector<index_type> marker(B.cols_, -1);
            
            for (size_t i = r_start; i < r_end; ++i) {
                std::vector<size_t> active_cols;

                for (size_type idx_a = A.row_ptr_[i]; idx_a < A.row_ptr_[i+1]; ++idx_a) {
                    size_type k = A.col_idx_[idx_a];
                    T val_a = A.values_[idx_a];

                    for (size_type idx_b = B.row_ptr_[k]; idx_b < B.row_ptr_[k+1]; ++idx_b) {
                        size_type j = B.col_idx_[idx_b];
                        T val_b = B.values_[idx_b];

                        if (marker[j] != (index_type)i) {
                            marker[j] = (index_type)i;
                            dense_acc[j] = T{};
                            active_cols.push_back(j);
                        }
                        dense_acc[j] += val_a * val_b;
                    }
                }

                if (active_cols.size() > limit) {
                    std::nth_element(active_cols.begin(), active_cols.begin() + limit, 
                                    active_cols.end(),
                        [&](size_t a, size_t b) { return std::abs(dense_acc[a]) > std::abs(dense_acc[b]); });
                    active_cols.resize(limit);
                }

                for (size_t col : active_cols) {
                    if (std::abs(dense_acc[col]) > 1e-9) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = col; t->value = dense_acc[col]; t->is_set = false;
                    }
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
                            t->row = i; t->col = ca; t->value = res;
                        }
                        idx_a++;
                    } else if (cb < ca) {
                        T res = op(T{}, B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = cb; t->value = res;
                        }
                        idx_b++;
                    } else {
                        T res = op(A.values_[idx_a], B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = ca; t->value = res;
                        }
                        idx_a++; idx_b++;
                    }
                }
                
                while (idx_a < end_a) {
                    T res = op(A.values_[idx_a], T{});
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = A.col_idx_[idx_a]; t->value = res;
                    }
                    idx_a++;
                }
                
                while (idx_b < end_b) {
                    T res = op(T{}, B.values_[idx_b]);
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = B.col_idx_[idx_b]; t->value = res;
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
                            t->row = i; t->col = col; t->value = A.values_[idx] * val_b;
                        }
                    }
                } else {
                    for (size_type idx = B.row_ptr_[i]; idx < B.row_ptr_[i+1]; ++idx) {
                        size_type col = B.col_idx_[idx];
                        T val_a = A.get(i, col);
                        if (val_a != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = col; t->value = val_a * B.values_[idx];
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
    std::vector<Triplet> temp_triplets_;
};

#endif // SPARSE_MATRIX_HPP