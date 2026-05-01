/**
 * @file Cprr.hpp
 * @brief Eigen-based implementation of CprrEigen for baseline comparison
 * @note This uses Eigen's standard SpGEMM + manual pruning (the "naive" approach)
 */

#ifndef CPRR_EIGEN_HPP
#define CPRR_EIGEN_HPP

#include "Method.hpp"
#include "MethodFactory.hpp"
#include <Eigen/Sparse>
#include <iostream>
#include <vector>
#include <algorithm>
#include <omp.h>

class CprrEigen : public Method {
public:
    using SpMat = Eigen::SparseMatrix<float, Eigen::RowMajor>;
    using Triplet = Eigen::Triplet<float>;
    
    explicit CprrEigen() : Method() {}

protected:
    void preprocess() override {
        l = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_EIGEN_L"));
        k = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_EIGEN_K"));
        t = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_EIGEN_T"));
        
        if (l > n) throw std::runtime_error("L > N");
        if (k > l) throw std::runtime_error("K > L");
        
        dataProvider.setL(l);
        
        // Pre-allocate Eigen matrices
        W.resize(n, n);
        W.reserve(Eigen::VectorXi::Constant(n, k)); // Reserve k entries per row
        
        logMessage("[Cprr] Initialized (Eigen Baseline)");
    }

    void execute() override {
        for (int curIteration = 0; curIteration < t; curIteration++) {
            std::cout << "\n[Cprr]: Iteration " << (curIteration + 1) << std::endl;

            // 1. Construct Weight Matrix W
            buildWeightMatrix();

            // 2. Transpose
            auto start_transpose = std::chrono::high_resolution_clock::now();
            SpMat WT = W.transpose();
            auto end_transpose = std::chrono::high_resolution_clock::now();
            std::cout << "  Transpose: " 
                      << std::chrono::duration<double>(end_transpose - start_transpose).count() 
                      << "s" << std::endl;

            // 3. SpGEMM: MA = WT * W (This is where Eigen might explode memory)
            auto start_ma = std::chrono::high_resolution_clock::now();
            SpMat MA_full = WT * W; // CRITICAL: Full multiplication before pruning
            SpMat MA = pruneMatrix(MA_full, l);
            auto end_ma = std::chrono::high_resolution_clock::now();
            std::cout << "  MA = WT*W (full NNZ: " << MA_full.nonZeros() 
                      << ", pruned: " << MA.nonZeros() << "): "
                      << std::chrono::duration<double>(end_ma - start_ma).count() 
                      << "s" << std::endl;
            
            // Clear intermediate to free memory
            SpMat().swap(MA_full);

            // 4. SpGEMM: MB = W * WT
            auto start_mb = std::chrono::high_resolution_clock::now();
            SpMat MB_full = W * WT; // CRITICAL: Full multiplication before pruning
            SpMat MB = pruneMatrix(MB_full, l);
            auto end_mb = std::chrono::high_resolution_clock::now();
            std::cout << "  MB = W*WT (full NNZ: " << MB_full.nonZeros() 
                      << ", pruned: " << MB.nonZeros() << "): "
                      << std::chrono::duration<double>(end_mb - start_mb).count() 
                      << "s" << std::endl;
            
            SpMat().swap(MB_full);

            // 5. Merge: finalMat = MA + MB
            auto start_merge = std::chrono::high_resolution_clock::now();
            SpMat finalMat = MA + MB;
            auto end_merge = std::chrono::high_resolution_clock::now();
            std::cout << "  Merge (NNZ: " << finalMat.nonZeros() << "): "
                      << std::chrono::duration<double>(end_merge - start_merge).count() 
                      << "s" << std::endl;

            // 6. Update Ranked Lists
            updateRankedLists(finalMat);
        }
    }

    void postprocess() override {}

private:
    void buildWeightMatrix() {
        std::vector<Triplet> triplets;
        triplets.reserve(n * k);

        // Sequential construction (Eigen doesn't have great parallel triplet insertion)
        for (size_t i = 0; i < n; ++i) {
            for (int j = 0; j < k; ++j) {
                uint32_t neighbor = (*rkLists)[i * l + j];
                float weight = static_cast<float>(k - j);
                triplets.emplace_back(i, neighbor, weight);
            }
        }

        W.setFromTriplets(triplets.begin(), triplets.end());
        W.makeCompressed(); // Convert to CSR
    }

    /**
     * @brief Prunes each row to top-L entries by absolute value
     * @note This is the CRITICAL bottleneck: requires iterating full matrix
     */
    SpMat pruneMatrix(const SpMat& mat, size_t limit) {
        std::vector<Triplet> pruned_triplets;
        pruned_triplets.reserve(n * limit);

        #pragma omp parallel
        {
            std::vector<Triplet> local_triplets;
            local_triplets.reserve(limit * 100); // Local buffer

            #pragma omp for schedule(dynamic, 64)
            for (int i = 0; i < mat.outerSize(); ++i) {
                std::vector<std::pair<float, int>> row_values;
                
                // Extract row
                for (SpMat::InnerIterator it(mat, i); it; ++it) {
                    row_values.emplace_back(std::abs(it.value()), it.col());
                }

                // Partial sort to get top-L
                if (row_values.size() > limit) {
                    std::partial_sort(
                        row_values.begin(), 
                        row_values.begin() + limit,
                        row_values.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; }
                    );
                    row_values.resize(limit);
                }

                // Emit triplets
                for (const auto& [val, col] : row_values) {
                    local_triplets.emplace_back(i, col, val);
                }
            }

            #pragma omp critical
            {
                pruned_triplets.insert(
                    pruned_triplets.end(),
                    local_triplets.begin(),
                    local_triplets.end()
                );
            }
        }

        SpMat result(mat.rows(), mat.cols());
        result.setFromTriplets(pruned_triplets.begin(), pruned_triplets.end());
        result.makeCompressed();
        return result;
    }

    void updateRankedLists(const SpMat& mat) {
        #pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<float, int>> scores;
            
            for (SpMat::InnerIterator it(mat, i); it; ++it) {
                scores.emplace_back(it.value(), it.col());
            }

            std::sort(scores.begin(), scores.end(), 
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            int limit = std::min(static_cast<int>(scores.size()), static_cast<int>(l));
            for (int j = 0; j < limit; ++j) {
                (*rkLists)[i * l + j] = scores[j].second;
            }
            
            // Ensure query image is at position 0
            int q_pos = -1;
            for (int j = 0; j < limit; ++j) {
                if ((*rkLists)[i * l + j] == i) {
                    q_pos = j;
                    break;
                }
            }
            
            if (q_pos > 0) {
                std::swap((*rkLists)[i * l], (*rkLists)[i * l + q_pos]);
            } else if (q_pos == -1) {
                for (int j = l - 1; j > 0; --j) {
                    (*rkLists)[i * l + j] = (*rkLists)[i * l + j - 1];
                }
                (*rkLists)[i * l] = i;
            }
        }
    }

    uint32_t l, k, t;
    SpMat W;
    
    static MethodRegister<CprrEigen> signInMethod;
};

#endif // CPRR_EIGEN_HPP