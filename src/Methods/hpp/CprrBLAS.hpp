/**
 * @file CprrBLAS.hpp
 * @brief GraphBLAS baseline for CPRR algorithm.
 * 
 * INTENTIONAL LIMITATIONS (demonstrating the problem our work solves):
 * - SpGEMM and eWiseAdd materialize full sparsity union
 * - MA + MB can approach O(N²) density for large datasets
 * - No mechanism for online pruning during matrix operations
 * 
 * Expected behavior: OOM failure for N > 100K on systems with < 32 GB RAM.
 * This empirically validates our claim that general-purpose graph libraries
 * cannot handle UDLF's requirement for bounded intermediate matrices.
 */

#ifndef CPRR_GRAPHBLAS_HPP
#define CPRR_GRAPHBLAS_HPP

#include "Method.hpp"
extern "C" {
	#include <GraphBLAS.h>
}
#include <iostream>
#include <vector>

// Error checking macro
// Error checking macro
#ifdef GRB_CHECK
#undef GRB_CHECK
#endif
#define GRB_CHECK(call) \
    do { \
        GrB_Info info = (call); \
        if (info != GrB_SUCCESS) { \
            std::cerr << "GraphBLAS Error at " << __FILE__ << ":" << __LINE__ << " Code: " << info << std::endl; \
            if (info == GrB_OUT_OF_MEMORY) { \
                std::cerr << "OUT OF MEMORY - Expected for N > 100K" << std::endl; \
                std::cerr << "This demonstrates the sparsity union problem in eWiseAdd" << std::endl; \
            } \
            exit(1); \
        } \
    } while(0)

class CprrBLAS : public Method {
public:
    explicit CprrBLAS() : Method() {}

    ~CprrBLAS() {
        // Cleanup GraphBLAS objects if they were created
        if (matrices_created) {
            GrB_Matrix_free(&W);
            GrB_Matrix_free(&WT);
            GrB_Matrix_free(&MA);
            GrB_Matrix_free(&MB);
            GrB_Matrix_free(&Final);
        }
        GrB_finalize();
    }

protected:
    void preprocess() override {
        l = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_BLAS_L"));
        k = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_BLAS_K"));
        t = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_BLAS_T"));

        dataProvider.setL(l);
        
        GRB_CHECK(GrB_init(GrB_NONBLOCKING));
        
        logMessage("[Cprr-GrB] Initialized (GraphBLAS Baseline)");
        logMessage("[Cprr-GrB] WARNING: Will materialize full MA + MB union");
    }

    void execute() override {
        for (int curIteration = 0; curIteration < t; curIteration++) {
            std::cout << "\n[Cprr-GrB]: Iteration " << (curIteration + 1) 
                      << "/" << t << std::endl;

            // 1. Build W from Ranked Lists
            buildGraphBLASMatrix();

            // 2. Transpose W -> WT
            GRB_CHECK(GrB_Matrix_new(&WT, GrB_FP32, n, n));
            GRB_CHECK(GrB_transpose(WT, NULL, NULL, W, NULL));

            std::cout << "[Cprr-GrB] Computing MA = WT * W (may OOM)..." << std::endl;

            // 3. SpGEMM: MA = WT * W
            GRB_CHECK(GrB_Matrix_new(&MA, GrB_FP32, n, n));
            GRB_CHECK(GrB_mxm(MA, NULL, NULL, GxB_PLUS_TIMES_FP32, WT, W, NULL));

            std::cout << "[Cprr-GrB] Computing MB = W * WT (may OOM)..." << std::endl;

            // 4. SpGEMM: MB = W * WT
            GRB_CHECK(GrB_Matrix_new(&MB, GrB_FP32, n, n));
            GRB_CHECK(GrB_mxm(MB, NULL, NULL, GxB_PLUS_TIMES_FP32, W, WT, NULL));

            // 5. Merge: Final = MA + MB
            // CRITICAL: GraphBLAS computes the union of sparsity patterns.
            // If MA and MB have different patterns (which they do), 
            // Final can approach 100% density → O(N²) storage.
            // For VegFru (N=160K), this creates 25.6 billion potential entries.
            std::cout << "[Cprr-GrB] Computing Final = MA + MB (CRITICAL: full union)..." << std::endl;
            
            GRB_CHECK(GrB_Matrix_new(&Final, GrB_FP32, n, n));
            GRB_CHECK(GrB_Matrix_eWiseAdd_BinaryOp(Final, NULL, NULL, 
                                                    GrB_PLUS_FP32, MA, MB, NULL));

            std::cout << "[Cprr-GrB] Operations complete. Extracting results..." << std::endl;

            // 6. Extraction & Pruning (too late to prevent OOM)
            updateRankedListsFromGrB(Final);

            // Cleanup for next iteration
            GRB_CHECK(GrB_Matrix_free(&W));
            GRB_CHECK(GrB_Matrix_free(&WT));
            GRB_CHECK(GrB_Matrix_free(&MA));
            GRB_CHECK(GrB_Matrix_free(&MB));
            GRB_CHECK(GrB_Matrix_free(&Final));
            matrices_created = false;
        }
    }

    void postprocess() override {
        logMessage("[Cprr-GrB] Completed");
    }

private:
    GrB_Matrix W, WT, MA, MB, Final;
    uint32_t l, k, t;
    bool matrices_created = false;

    void buildGraphBLASMatrix() {
        GRB_CHECK(GrB_Matrix_new(&W, GrB_FP32, n, n));
        matrices_created = true;
        
        // Use vectors instead of arrays for cleaner memory management
        std::vector<GrB_Index> rows, cols;
        std::vector<float> vals;
        rows.reserve(n * k);
        cols.reserve(n * k);
        vals.reserve(n * k);

        for (size_t i = 0; i < n; ++i) {
            for (int j = 0; j < k; ++j) {
                rows.push_back(i);
                cols.push_back((*rkLists)[i * l + j]);
                vals.push_back((float)(k - j));
            }
        }

        GRB_CHECK(GrB_Matrix_build_FP32(W, rows.data(), cols.data(), 
                                        vals.data(), rows.size(), 
                                        GrB_PLUS_FP32));
    }

    void updateRankedListsFromGrB(GrB_Matrix& mat) {
        GrB_Index nvals;
        GRB_CHECK(GrB_Matrix_nvals(&nvals, mat));

        std::cout << "[Cprr-GrB] Extracting " << nvals << " triplets..." << std::endl;

        // Extract all triplets
        std::vector<GrB_Index> rows(nvals), cols(nvals);
        std::vector<float> vals(nvals);
        
        GRB_CHECK(GrB_Matrix_extractTuples_FP32(rows.data(), cols.data(), 
                                                vals.data(), &nvals, mat));

        // Convert to per-row adjacency lists
        std::vector<std::vector<std::pair<float, int>>> row_data(n);
        for(size_t i = 0; i < nvals; ++i) {
            row_data[rows[i]].push_back({vals[i], (int)cols[i]});
        }

        // Parallel sort and update
        #pragma omp parallel for schedule(dynamic, 64)
        for (int i = 0; i < n; ++i) {
            // Sort by score descending
            std::sort(row_data[i].begin(), row_data[i].end(), 
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            // Take top-L
            int limit = std::min((int)row_data[i].size(), (int)l);
            for(int j = 0; j < limit; ++j) {
                (*rkLists)[i * l + j] = row_data[i][j].second;
            }
            
            // Ensure query image is at position 0
            int q_pos = -1;
            for(int j = 0; j < limit; ++j) {
                if((*rkLists)[i * l + j] == i) {
                    q_pos = j;
                    break;
                }
            }
            
            if(q_pos > 0) {
                std::swap((*rkLists)[i * l], (*rkLists)[i * l + q_pos]);
            } else if(q_pos == -1) {
                // Query not in top-L, insert at position 0
                for(int j = l - 1; j > 0; --j) {
                    (*rkLists)[i * l + j] = (*rkLists)[i * l + j - 1];
                }
                (*rkLists)[i * l] = i;
            }
        }
    }
    
    static MethodRegister<CprrBLAS> signInMethod;
};

#endif
