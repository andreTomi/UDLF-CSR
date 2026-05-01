/**
 * @file LHRR.hpp
 * @brief Sparse Matrix optimized implementation of LHRR (Row-Split + BlockAllocator)
 */

#ifndef LHRR_SPARSE_HPP
#define LHRR_SPARSE_HPP

#include "Method.hpp"
#include "MethodFactory.hpp"
#include "SparseMatrix.hpp"
#include "BlockAllocator.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>
#include <omp.h>

class LHRRSparse : public Method {
public:
    explicit LHRRSparse() : Method() {}

protected:
    void preprocess() override {
        l = config.get<int>("PARAM_LHRR_SPARSE_L");
        k = config.get<int>("PARAM_LHRR_SPARSE_K");
        t = config.get<int>("PARAM_LHRR_SPARSE_T");
        if (l > n) throw std::runtime_error("L > N");
        
        dataProvider.setL(l);
        logK = std::log((float)k);
        logMessage("[LHRR] Initialized (Sparse Mode + BlockAllocator)");
    }

    void execute() override {
        CSRMatrix<float> mainMatrix(n, n, 0);
        CSRMatrix<float> H(n, n, 0);
        int max_threads = omp_get_max_threads();

        // 1. Fill Position Matrix (Row-Split)
        {
            logMessage("Initializing Position Matrix...");
            mainMatrix.reset();
            
            std::vector<CSRMatrix<float>::PaddedAllocator> allocators(max_threads);
            
            // Ranges needed for the build function
            std::vector<std::pair<CSRMatrix<float>::size_type, CSRMatrix<float>::size_type>> ranges(max_threads);

            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                auto& allocator = *allocators[tid].ptr;

                // Standard block partition
                size_t chunk = (n + max_threads - 1) / max_threads;
                size_t r_start = std::min((size_t)n, tid * chunk);
                size_t r_end   = std::min((size_t)n, (tid + 1) * chunk);
                ranges[tid] = {r_start, r_end};

                #pragma omp for schedule(dynamic)
                for (int i = 0; i < n; ++i) {
                    for (int j = 0; j < l; ++j) {
                        uint32_t neighbor = (*rkLists)[i * l + j];
                        float w = (float)(l - j);
                        
                        // Emit Forward
                        auto* t1 = allocator.allocate();
                        t1->row = i; t1->col = neighbor; t1->value = w; t1->is_set = false;

                        // Emit Backward (Symmetric)
                        // Even though 'neighbor' might be owned by another thread,
                        // the global sort in build_from_partial_results will route it correctly.
                        auto* t2 = allocator.allocate();
                        t2->row = neighbor; t2->col = i; t2->value = w; t2->is_set = false;
                    }
                }
            }
            
            // This now works perfectly for symmetric updates
            CSRMatrix<float>::build_from_partial_results(mainMatrix, allocators, ranges);
            
            updateRankedLists(mainMatrix);
        }

        // 2. Iterations
        for (int iter = 1; iter <= t; ++iter) {
            std::cout << "\n -> Iteration " << iter << std::endl;

            // A. Build Hyperedge Matrix (H) - Row-Split Friendly!
            // -----------------------------------------------------------------
            H.reset();
            std::vector<CSRMatrix<float>::PaddedAllocator> allocators(max_threads);
            
            std::vector<std::pair<CSRMatrix<float>::size_type, CSRMatrix<float>::size_type>> ranges(max_threads);

            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                auto& allocator = *allocators[tid].ptr;
                
                size_t chunk = (n + max_threads - 1) / max_threads;
                size_t r_start = std::min((size_t)n, tid * chunk);
                size_t r_end   = std::min((size_t)n, (tid + 1) * chunk);
                ranges[tid] = {r_start, r_end};
                
                for (size_t i = r_start; i < r_end; ++i) {
                    auto* t = allocator.allocate();
                    t->row = i; t->col = i; t->value = 1.0f; t->is_set = false;

                    for (int o = 0; o < k; ++o) {
                        int imgO = (*rkLists)[i * l + o];
                        float wO = weightPosition(o + 1);

                        for (int j = 0; j < k; ++j) {
                            int imgJ = (*rkLists)[imgO * l + j];
                            float wJ = weightPosition(j + 1);
                            
                            auto* t2 = allocator.allocate();
                            t2->row = i; 
                            t2->col = imgJ; 
                            t2->value = wO * wJ; 
                            t2->is_set = false;
                        }
                    }
                }
            }
            // H construction is perfectly row-local. Use Fast Path.
            CSRMatrix<float>::build_from_partial_results(H, allocators, ranges);

            // B. Algebraic Pipeline (All use Row-Split internally)
            CSRMatrix<float> H_T = H.transpose();
            CSRMatrix<float> S = CSRMatrix<float>::multiply_and_prune(H, H_T, l);
            CSRMatrix<float> S_rev = CSRMatrix<float>::multiply_and_prune(H_T, H, l);

            CSRMatrix<float> S_total = CSRMatrix<float>::combine(S, S_rev);
            mainMatrix = CSRMatrix<float>::combine(mainMatrix, S_total);
            
            updateRankedLists(mainMatrix);
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
            std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { 
                return a.first > b.first; 
            });
            for(int j=0; j < std::min((int)scores.size(), l); ++j) {
                (*rkLists)[i * l + j] = scores[j].second;
            }
            int q_pos = -1;
            for(int j=0; j<l; ++j) if((*rkLists)[i*l+j]==i) q_pos=j;
            if(q_pos > 0) std::swap((*rkLists)[i*l], (*rkLists)[i*l+q_pos]);
            else if(q_pos == -1) {
                for(int j=l-1; j>0; --j) (*rkLists)[i*l+j] = (*rkLists)[i*l+j-1];
                (*rkLists)[i*l] = i;
            }
        }
    }

    float weightPosition(int pos) {
        return (pos == 1) ? 1.0f : (1.0f - std::log((float)pos) / logK);
    }

    int l, k, t;
    float logK;
    static MethodRegister<LHRRSparse> signInMethod;
};

#endif // LHRR_SPARSE_HPP