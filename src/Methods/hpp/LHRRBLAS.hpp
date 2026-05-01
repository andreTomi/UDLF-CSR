/**
 * @file LHRRBLAS.hpp
 * @brief GraphBLAS implementation of LHRR - CORRECTED VERSION.
 * 
 * CRITICAL FIX: Uses Hadamard product (element-wise multiply) for accumulation,
 * matching the CSR implementation's combine() operation.
 * 
 * Memory warning still applies - H = Wk * Wk creates O(N·k²) entries.
 */

#ifndef LHRR_GRAPHBLAS_HPP
#define LHRR_GRAPHBLAS_HPP

extern "C" {
	#include <GraphBLAS.h>
}
#include "Method.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>

#ifdef GRB_CHECK
#undef GRB_CHECK
#endif
#define GRB_CHECK(call) \
    do { \
        GrB_Info info = (call); \
        if (info != GrB_SUCCESS) { \
            std::cerr << "GraphBLAS Error at " << __FILE__ << ":" << __LINE__ \
                      << " Code: " << info << std::endl; \
            if (info == GrB_OUT_OF_MEMORY) { \
                std::cerr << "OUT OF MEMORY - Expected for LHRR with N > 80K" << std::endl; \
            } \
            exit(1); \
        } \
    } while(0)

class LHRRBLAS : public Method {
public:
    explicit LHRRBLAS() : Method() {}
    
    ~LHRRBLAS() {
        if (matrices_initialized) {
            GrB_Matrix_free(&MainMat);
            GrB_Matrix_free(&H);
            GrB_Matrix_free(&HT);
            GrB_Matrix_free(&S);
            GrB_Matrix_free(&S_Rev);
            GrB_Matrix_free(&Temp);
        }
        GrB_finalize();
    }

protected:
    void preprocess() override {
        l = config.get<int>("PARAM_LHRR_BLAS_L");
        k = config.get<int>("PARAM_LHRR_BLAS_K");
        t = config.get<int>("PARAM_LHRR_BLAS_T");
        logK = std::log((float)k);

        GRB_CHECK(GrB_init(GrB_NONBLOCKING));
        
        // CRITICAL: Must set L in dataProvider for VirtualArray
        dataProvider.setL(l);
        
        logMessage("[LHRR-GrB] Initialized (Corrected Implementation)");
        std::cout << "[LHRR-GrB] Using Hadamard product for accumulation" << std::endl;
    }

    void execute() override {
        // Phase 1: Initialize symmetric position matrix
        logMessage("Initializing Position Matrix...");
        
        GrB_Matrix W = buildWeightMatrix(l, [](int rank, int limit) {
             return (float)(limit - rank); 
        });

        // Symmetrize: MainMat = W + W^T
        GRB_CHECK(GrB_Matrix_new(&MainMat, GrB_FP32, n, n));
        GRB_CHECK(GrB_Matrix_new(&Temp, GrB_FP32, n, n));
        GRB_CHECK(GrB_transpose(Temp, NULL, NULL, W, NULL));
        GRB_CHECK(GrB_Matrix_eWiseAdd_BinaryOp(MainMat, NULL, NULL, 
                                                GrB_PLUS_FP32, W, Temp, NULL));
        
        matrices_initialized = true;
        updateRankedListsFromGrB(MainMat);
        
        GRB_CHECK(GrB_Matrix_free(&W));
        GRB_CHECK(GrB_Matrix_free(&Temp));

        // Phase 2: Iterations
        for (int iter = 1; iter <= t; ++iter) {
            std::cout << "\n[LHRR-GrB]: Iteration " << iter << "/" << t << std::endl;

            // Build hyperedge matrix Wk
            std::cout << "[LHRR-GrB] Building Wk matrix..." << std::endl;
            GrB_Matrix Wk = buildWeightMatrix(k, [&](int rank, int limit) {
                return weightPosition(rank + 1);
            });

            // H = Wk * Wk
            std::cout << "[LHRR-GrB] Computing H = Wk * Wk..." << std::endl;
            GRB_CHECK(GrB_Matrix_new(&H, GrB_FP32, n, n));
            GRB_CHECK(GrB_mxm(H, NULL, NULL, GxB_PLUS_TIMES_FP32, Wk, Wk, NULL));
            GRB_CHECK(GrB_Matrix_free(&Wk));

            // Add self-loops to H
            std::cout << "[LHRR-GrB] Adding self-loops to H..." << std::endl;
            GrB_Matrix IdMat;
            GRB_CHECK(GrB_Matrix_new(&IdMat, GrB_FP32, n, n));
            
            std::vector<GrB_Index> i_rows(n), i_cols(n);
            std::vector<float> i_vals(n, 1.0f);
            for(size_t i = 0; i < n; ++i) { 
                i_rows[i] = i; 
                i_cols[i] = i; 
            }
            
            GRB_CHECK(GrB_Matrix_build_FP32(IdMat, i_rows.data(), i_cols.data(), 
                                            i_vals.data(), n, GrB_PLUS_FP32));
            GRB_CHECK(GrB_Matrix_eWiseAdd_BinaryOp(H, NULL, NULL, 
                                                    GrB_PLUS_FP32, H, IdMat, NULL));
            GRB_CHECK(GrB_Matrix_free(&IdMat));

            // Compute S = H * H^T
            std::cout << "[LHRR-GrB] Computing S = H * H^T..." << std::endl;
            GRB_CHECK(GrB_Matrix_new(&HT, GrB_FP32, n, n));
            GRB_CHECK(GrB_transpose(HT, NULL, NULL, H, NULL));
            GRB_CHECK(GrB_Matrix_new(&S, GrB_FP32, n, n));
            GRB_CHECK(GrB_mxm(S, NULL, NULL, GxB_PLUS_TIMES_FP32, H, HT, NULL));

            // Compute S_Rev = H^T * H
            std::cout << "[LHRR-GrB] Computing S_Rev = H^T * H..." << std::endl;
            GRB_CHECK(GrB_Matrix_new(&S_Rev, GrB_FP32, n, n));
            GRB_CHECK(GrB_mxm(S_Rev, NULL, NULL, GxB_PLUS_TIMES_FP32, HT, H, NULL));

            // CRITICAL FIX: Hadamard product S_total = S ⊙ S_rev
            std::cout << "[LHRR-GrB] Computing S_total = S ⊙ S_Rev (Hadamard)..." << std::endl;
            GrB_Matrix S_total;
            GRB_CHECK(GrB_Matrix_new(&S_total, GrB_FP32, n, n));
            GRB_CHECK(GrB_Matrix_eWiseMult_BinaryOp(S_total, NULL, NULL,
                                                     GrB_TIMES_FP32, S, S_Rev, NULL));

            // CRITICAL FIX: Hadamard product MainMat = MainMat ⊙ S_total
            std::cout << "[LHRR-GrB] Updating MainMat = MainMat ⊙ S_total (Hadamard)..." << std::endl;
            GrB_Matrix MainMat_new;
            GRB_CHECK(GrB_Matrix_new(&MainMat_new, GrB_FP32, n, n));
            GRB_CHECK(GrB_Matrix_eWiseMult_BinaryOp(MainMat_new, NULL, NULL,
                                                     GrB_TIMES_FP32, MainMat, S_total, NULL));
            
            GRB_CHECK(GrB_Matrix_free(&MainMat));
            MainMat = MainMat_new;

            // Update ranked lists
            std::cout << "[LHRR-GrB] Extracting and pruning results..." << std::endl;
            updateRankedListsFromGrB(MainMat);

            // Cleanup
            GRB_CHECK(GrB_Matrix_free(&H));
            GRB_CHECK(GrB_Matrix_free(&HT));
            GRB_CHECK(GrB_Matrix_free(&S));
            GRB_CHECK(GrB_Matrix_free(&S_Rev));
            GRB_CHECK(GrB_Matrix_free(&S_total));
        }
    }

    void postprocess() override {
        logMessage("[LHRR-GrB] Completed");
    }

private:
    GrB_Matrix MainMat, H, HT, S, S_Rev, Temp;
    int l, k, t;
    float logK;
    bool matrices_initialized = false;

    template<typename Func>
    GrB_Matrix buildWeightMatrix(int limit, Func weightCalc) {
        GrB_Matrix M;
        GRB_CHECK(GrB_Matrix_new(&M, GrB_FP32, n, n));
        
        std::vector<GrB_Index> rows, cols;
        std::vector<float> vals;
        rows.reserve(n * limit);
        cols.reserve(n * limit);
        vals.reserve(n * limit);

        for(int i = 0; i < n; ++i) {
            for(int j = 0; j < limit; ++j) {
                rows.push_back(i);
                cols.push_back((*rkLists)[i * l + j]);
                vals.push_back(weightCalc(j, limit));
            }
        }
        
        GRB_CHECK(GrB_Matrix_build_FP32(M, rows.data(), cols.data(), 
                                        vals.data(), rows.size(), 
                                        GrB_PLUS_FP32));
        return M;
    }

    void updateRankedListsFromGrB(GrB_Matrix& mat) {
        GrB_Index nvals;
        GRB_CHECK(GrB_Matrix_nvals(&nvals, mat));
        
        std::cout << "[LHRR-GrB] Extracting " << nvals << " triplets..." << std::endl;

        std::vector<GrB_Index> rows(nvals), cols(nvals);
        std::vector<float> vals(nvals);
        GRB_CHECK(GrB_Matrix_extractTuples_FP32(rows.data(), cols.data(), 
                                                vals.data(), &nvals, mat));

        std::vector<std::vector<std::pair<float, int>>> row_data(n);
        for(size_t i = 0; i < nvals; ++i) {
            row_data[rows[i]].push_back({vals[i], (int)cols[i]});
        }

        #pragma omp parallel for schedule(dynamic, 64)
        for(int i = 0; i < n; ++i) {
            std::sort(row_data[i].begin(), row_data[i].end(), 
                      [](auto& a, auto& b){ return a.first > b.first; });
            
            int lim = std::min((int)row_data[i].size(), l);
            for(int j = 0; j < lim; ++j) {
                (*rkLists)[i * l + j] = row_data[i][j].second;
            }
            
            // Self-correction
            int q_pos = -1;
            for(int j = 0; j < lim; ++j) {
                if((*rkLists)[i * l + j] == i) {
                    q_pos = j;
                    break;
                }
            }
            
            if(q_pos > 0) {
                std::swap((*rkLists)[i * l], (*rkLists)[i * l + q_pos]);
            } else if(q_pos == -1) {
                for(int j = l - 1; j > 0; --j) {
                    (*rkLists)[i * l + j] = (*rkLists)[i * l + j - 1];
                }
                (*rkLists)[i * l] = i;
            }
        }
    }

    float weightPosition(int pos) {
        return (pos == 1) ? 1.0f : (1.0f - std::log((float)pos) / logK);
    }
    
    static MethodRegister<LHRRBLAS> signInMethod;
};

#endif
