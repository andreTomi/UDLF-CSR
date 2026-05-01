/**
 * @file Cprr.hpp
 * @brief Sparse Matrix optimized implementation of CPRR (Row-Split + BlockAllocator)
 */

#ifndef CPRR_SPARSE_HPP
#define CPRR_SPARSE_HPP

#include "Method.hpp"
#include "MethodFactory.hpp"
#include "SparseMatrix.hpp"
#include "BlockAllocator.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <omp.h>

class CprrSparse : public Method {
public:
    explicit CprrSparse() : Method() {}

protected:
    void preprocess() override {
        l = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_SPARSE_L"));
        k = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_SPARSE_K"));
        t = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_SPARSE_T"));
        
        if (l > n) throw std::runtime_error("L > N");
        if (k > l) throw std::runtime_error("K > L");
        
        dataProvider.setL(l);
        logMessage("[Cprr] Initialized (Algebraic Mode + BlockAllocator)");
    }

    void execute() override {
        CSRMatrix<float> W(n, n, 0); // No pre-alloc needed
        int max_threads = omp_get_max_threads();

        for (int curIteration = 0; curIteration < t; curIteration++) {
            std::cout << "\n[Cprr]: Iteration " << (curIteration + 1) << " (SpGEMM)" << std::endl;

            // 1. Construct Weight Matrix W (Using Row-Split Pattern)
            W.reset();
            
            // Setup Allocators and Ranges
            std::vector<CSRMatrix<float>::PaddedAllocator> allocators(max_threads);
            
            std::vector<std::pair<CSRMatrix<float>::size_type, CSRMatrix<float>::size_type>> ranges(max_threads);

            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                auto& allocator = *allocators[tid].ptr;

                // Static Row Partitioning for Row-Split Strategy
                size_t chunk = (n + max_threads - 1) / max_threads;
                size_t r_start = std::min((size_t)n, tid * chunk);
                size_t r_end   = std::min((size_t)n, (tid + 1) * chunk);
                ranges[tid] = {r_start, r_end};

                // Loop only assigned rows
                for (size_t i = r_start; i < r_end; ++i) {
                    for (int j = 0; j < k; ++j) {
                        uint32_t neighbor = (*rkLists)[i * l + j];
                        float weight = (float)(k - j);
                        
                        auto* t = allocator.allocate();
                        t->row = i;
                        t->col = neighbor;
                        t->value = weight;
                        t->is_set = false;
                    }
                }
            }
            
            // Wait-Free Merge using Row-Split
            CSRMatrix<float>::build_from_partial_results(W, allocators, ranges);

            // 2. Algebraic Operations (Internal BlockAllocators are used automatically)
            CSRMatrix<float> WT = W.transpose();
            
            // These operations now use 'multiply_and_prune' which internally uses 
            // the Row-Split pattern + BlockAllocators.
            CSRMatrix<float> MA = CSRMatrix<float>::multiply_and_prune(WT, W, l);
            CSRMatrix<float> MB = CSRMatrix<float>::multiply_and_prune(W, WT, l);
            
            CSRMatrix<float> finalMat = CSRMatrix<float>::merge(MA, MB, 
                [](float a, float b) { return a + b; });

            // 3. Update Ranked Lists
            updateRankedLists(finalMat);
        }
    }

    void postprocess() override {}

private:
    void updateRankedLists(const CSRMatrix<float>& mat) {
        #pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<float, int>> scores;
            scores.reserve(mat.row_nnz(i));
            
            mat.for_each_in_row(i, [&](uint32_t col, float val){
                scores.push_back({val, col});
            });

            std::sort(scores.begin(), scores.end(), 
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            int limit = std::min((int)scores.size(), (int)l);
            for(int j=0; j<limit; ++j) {
                (*rkLists)[i * l + j] = scores[j].second;
            }
            
            int q_pos = -1;
            for(int j=0; j<limit; ++j) if((*rkLists)[i*l+j]==i) q_pos=j;
            if(q_pos > 0) std::swap((*rkLists)[i*l], (*rkLists)[i*l+q_pos]);
            else if(q_pos == -1) {
                for(int j=l-1; j>0; --j) (*rkLists)[i*l+j] = (*rkLists)[i*l+j-1];
                (*rkLists)[i*l] = i;
            }
        }
    }

    uint32_t l, k, t;
    static MethodRegister<CprrSparse> signInMethod;
};

#endif // CPRR_SPARSE_HPP