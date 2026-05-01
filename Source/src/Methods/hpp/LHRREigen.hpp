/**
 * @file LHRR.hpp
 * @brief Eigen-based implementation of LHRREigen for baseline comparison
 */

#ifndef LHRR_EIGEN_HPP
#define LHRR_EIGEN_HPP

#include "Method.hpp"
#include "MethodFactory.hpp"
#include <Eigen/Sparse>
#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>
#include <omp.h>

class LHRREigen : public Method {
public:
    using SpMat = Eigen::SparseMatrix<float, Eigen::RowMajor>;
    using Triplet = Eigen::Triplet<float>;
    
    explicit LHRREigen() : Method() {}

protected:
    void preprocess() override {
        l = config.get<int>("PARAM_LHRR_EIGEN_L");
        k = config.get<int>("PARAM_LHRR_EIGEN_K");
        t = config.get<int>("PARAM_LHRR_EIGEN_T");
        if (l > n) throw std::runtime_error("L > N");
        
        dataProvider.setL(l);
        logK = std::log(static_cast<float>(k));
        
        logMessage("[LHRR] Initialized (Eigen Baseline)");
    }

    void execute() override {
        // 1. Initialize Position Matrix
        {
            logMessage("Initializing Position Matrix...");
            mainMatrix = buildPositionMatrix();
            updateRankedLists(mainMatrix);
        }

        // 2. Iterative Refinement
        for (int iter = 1; iter <= t; ++iter) {
            std::cout << "\n -> Iteration " << iter << std::endl;

            // A. Build Hyperedge Matrix
            auto start_h = std::chrono::high_resolution_clock::now();
            SpMat H = buildHyperedgeMatrix();
            auto end_h = std::chrono::high_resolution_clock::now();
            std::cout << "  Build H (NNZ: " << H.nonZeros() << "): "
                      << std::chrono::duration<double>(end_h - start_h).count() 
                      << "s" << std::endl;

            // B. Transpose
            auto start_t = std::chrono::high_resolution_clock::now();
            SpMat H_T = H.transpose();
            auto end_t = std::chrono::high_resolution_clock::now();
            std::cout << "  Transpose: "
                      << std::chrono::duration<double>(end_t - start_t).count() 
                      << "s" << std::endl;

            // C. S = H * H^T (CRITICAL: Full product)
            auto start_s = std::chrono::high_resolution_clock::now();
            SpMat S_full = H * H_T;
            SpMat S = pruneMatrix(S_full, l);
            auto end_s = std::chrono::high_resolution_clock::now();
            std::cout << "  S = H*H^T (full NNZ: " << S_full.nonZeros() 
                      << ", pruned: " << S.nonZeros() << "): "
                      << std::chrono::duration<double>(end_s - start_s).count() 
                      << "s" << std::endl;
            SpMat().swap(S_full);

            // D. S_rev = H^T * H (CRITICAL: Full product)
            auto start_srev = std::chrono::high_resolution_clock::now();
            SpMat S_rev_full = H_T * H;
            SpMat S_rev = pruneMatrix(S_rev_full, l);
            auto end_srev = std::chrono::high_resolution_clock::now();
            std::cout << "  S_rev = H^T*H (full NNZ: " << S_rev_full.nonZeros() 
                      << ", pruned: " << S_rev.nonZeros() << "): "
                      << std::chrono::duration<double>(end_srev - start_srev).count() 
                      << "s" << std::endl;
            SpMat().swap(S_rev_full);

            // E. Hadamard product: S_total = S ⊙ S_rev
            auto start_had = std::chrono::high_resolution_clock::now();
            SpMat S_total = hadamardProduct(S, S_rev);
            auto end_had = std::chrono::high_resolution_clock::now();
            std::cout << "  Hadamard (NNZ: " << S_total.nonZeros() << "): "
                      << std::chrono::duration<double>(end_had - start_had).count() 
                      << "s" << std::endl;

            // F. Update main matrix: mainMatrix = mainMatrix ⊙ S_total
            auto start_update = std::chrono::high_resolution_clock::now();
            mainMatrix = hadamardProduct(mainMatrix, S_total);
            auto end_update = std::chrono::high_resolution_clock::now();
            std::cout << "  Update mainMatrix (NNZ: " << mainMatrix.nonZeros() << "): "
                      << std::chrono::duration<double>(end_update - start_update).count() 
                      << "s" << std::endl;

            updateRankedLists(mainMatrix);
        }
    }

    void postprocess() override {}

private:
    SpMat buildPositionMatrix() {
        std::vector<Triplet> triplets;
        triplets.reserve(2 * n * l); // Forward + Backward

        for (size_t i = 0; i < n; ++i) {
            for (int j = 0; j < l; ++j) {
                uint32_t neighbor = (*rkLists)[i * l + j];
                float w = static_cast<float>(l - j);
                
                // Forward
                triplets.emplace_back(i, neighbor, w);
                // Backward (symmetric)
                triplets.emplace_back(neighbor, i, w);
            }
        }

        SpMat mat(n, n);
        mat.setFromTriplets(triplets.begin(), triplets.end());
        mat.makeCompressed();
        return mat;
    }

    SpMat buildHyperedgeMatrix() {
        std::vector<Triplet> triplets;
        triplets.reserve(n * (1 + k * k));

        #pragma omp parallel
        {
            std::vector<Triplet> local_triplets;
            local_triplets.reserve((1 + k * k) * 100);

            #pragma omp for schedule(dynamic, 64)
            for (int i = 0; i < n; ++i) {
                // Self-connection
                local_triplets.emplace_back(i, i, 1.0f);

                // Two-hop neighbors
                for (int o = 0; o < k; ++o) {
                    int imgO = (*rkLists)[i * l + o];
                    float wO = weightPosition(o + 1);

                    for (int j = 0; j < k; ++j) {
                        int imgJ = (*rkLists)[imgO * l + j];
                        float wJ = weightPosition(j + 1);
                        
                        local_triplets.emplace_back(i, imgJ, wO * wJ);
                    }
                }
            }

            #pragma omp critical
            {
                triplets.insert(
                    triplets.end(),
                    local_triplets.begin(),
                    local_triplets.end()
                );
            }
        }

        SpMat H(n, n);
        H.setFromTriplets(triplets.begin(), triplets.end());
        H.makeCompressed();
        return H;
    }

    /**
     * @brief Hadamard (element-wise) product of two sparse matrices
     * @note Eigen doesn't have native Hadamard for sparse, so we implement it
     */
    SpMat hadamardProduct(const SpMat& A, const SpMat& B) {
        std::vector<Triplet> triplets;
        triplets.reserve(std::min(A.nonZeros(), B.nonZeros()));

        #pragma omp parallel
        {
            std::vector<Triplet> local_triplets;

            #pragma omp for schedule(dynamic, 64)
            for (int i = 0; i < A.outerSize(); ++i) {
                // Build map of B's row for fast lookup
                std::unordered_map<int, float> b_row;
                for (SpMat::InnerIterator it_b(B, i); it_b; ++it_b) {
                    b_row[it_b.col()] = it_b.value();
                }

                // Iterate A's row and multiply where both exist
                for (SpMat::InnerIterator it_a(A, i); it_a; ++it_a) {
                    auto it = b_row.find(it_a.col());
                    if (it != b_row.end()) {
                        float product = it_a.value() * it->second;
                        if (std::abs(product) > 1e-9f) {
                            local_triplets.emplace_back(i, it_a.col(), product);
                        }
                    }
                }
            }

            #pragma omp critical
            {
                triplets.insert(
                    triplets.end(),
                    local_triplets.begin(),
                    local_triplets.end()
                );
            }
        }

        SpMat result(A.rows(), A.cols());
        result.setFromTriplets(triplets.begin(), triplets.end());
        result.makeCompressed();
        return result;
    }

    SpMat pruneMatrix(const SpMat& mat, size_t limit) {
        std::vector<Triplet> pruned_triplets;
        pruned_triplets.reserve(n * limit);

        #pragma omp parallel
        {
            std::vector<Triplet> local_triplets;

            #pragma omp for schedule(dynamic, 64)
            for (int i = 0; i < mat.outerSize(); ++i) {
                std::vector<std::pair<float, int>> row_values;
                
                for (SpMat::InnerIterator it(mat, i); it; ++it) {
                    row_values.emplace_back(std::abs(it.value()), it.col());
                }

                if (row_values.size() > limit) {
                    std::partial_sort(
                        row_values.begin(), 
                        row_values.begin() + limit,
                        row_values.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; }
                    );
                    row_values.resize(limit);
                }

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

            int limit = std::min(static_cast<int>(scores.size()), l);
            for (int j = 0; j < limit; ++j) {
                (*rkLists)[i * l + j] = scores[j].second;
            }
            
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

    float weightPosition(int pos) {
        return (pos == 1) ? 1.0f : (1.0f - std::log(static_cast<float>(pos)) / logK);
    }

    int l, k, t;
    float logK;
    SpMat mainMatrix;
    
    static MethodRegister<LHRREigen> signInMethod;
};

#endif // LHRR_EIGEN_HPP