/**
 * @file CprrMKL.hpp
 * @brief Intel MKL baseline for CPRR algorithm - CORRECTED VERSION.
 */

#ifndef CPRR_MKL_HPP
#define CPRR_MKL_HPP

#include "Method.hpp"
#include <mkl.h>
#include <vector>
#include <algorithm>
#include <iostream>

#ifndef MKL_CHECK
#define MKL_CHECK(call, msg) \
    do { \
        sparse_status_t status = (call); \
        if (status != SPARSE_STATUS_SUCCESS) { \
            std::cerr << "MKL Error: " << msg << " (line " << __LINE__ << ")" << std::endl; \
            if (status == SPARSE_STATUS_ALLOC_FAILED) { \
                std::cerr << "OUT OF MEMORY" << std::endl; \
            } \
            exit(1); \
        } \
    } while(0)
#endif

class CprrMKL : public Method {
public:
    explicit CprrMKL() : Method() {}

protected:
    void preprocess() override {
        l = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_MKL_L"));
        k = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_MKL_K"));
        t = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_MKL_T"));
        
        // CRITICAL: Must set L in dataProvider for VirtualArray
        dataProvider.setL(l);
        
        logMessage("[Cprr-MKL] Initialized");
    }

    void execute() override {
        for (int curIteration = 0; curIteration < t; curIteration++) {
            std::cout << "\n[Cprr-MKL]: Iteration " << (curIteration + 1) << "/" << t << std::endl;

            // Build weight matrix W
            auto W_wrapper = buildWeightMatrix();
            sparse_matrix_t W = W_wrapper->handle;

            // Compute MA = WT * W
            std::cout << "[Cprr-MKL] Computing MA = WT * W..." << std::endl;
            sparse_matrix_t MA;
            MKL_CHECK(
                mkl_sparse_spmm(SPARSE_OPERATION_TRANSPOSE, W, W, &MA),
                "Failed MA = WT * W"
            );

            // Build WT explicitly for W * WT computation
            // We use our custom createTranspose which returns a wrapper
            std::cout << "[Cprr-MKL] Building WT..." << std::endl;
            auto WT_wrapper = createTranspose(W);
            sparse_matrix_t WT = WT_wrapper->handle;

            // Compute MB = W * WT
            std::cout << "[Cprr-MKL] Computing MB = W * WT..." << std::endl;
            sparse_matrix_t MB;
            MKL_CHECK(
                mkl_sparse_spmm(SPARSE_OPERATION_NON_TRANSPOSE, W, WT, &MB),
                "Failed MB = W * WT"
            );

            // Merge Final = MA + MB
            std::cout << "[Cprr-MKL] Computing Final = MA + MB..." << std::endl;
            sparse_matrix_t Final;
            MKL_CHECK(
                mkl_sparse_s_add(SPARSE_OPERATION_NON_TRANSPOSE, MA, 1.0f, MB, &Final),
                "Failed Final = MA + MB"
            );

            // Update ranked lists
            updateRankedLists(Final);

            // Cleanup
            // W and WT are handled by shared_ptr destructors
            mkl_sparse_destroy(MA);
            mkl_sparse_destroy(MB);
            mkl_sparse_destroy(Final);
        }
    }

    void postprocess() override {
        logMessage("[Cprr-MKL] Completed");
    }

private:
    uint32_t l, k, t;

    // Wrapper to keep data alive for MKL handles
    struct MKLData {
        std::vector<MKL_INT> row_ptr;
        std::vector<MKL_INT> col_ind;
        std::vector<float> values;
        sparse_matrix_t handle = nullptr;

        ~MKLData() {
            if (handle) mkl_sparse_destroy(handle);
        }

        // No copying to prevent double-free of handle
        MKLData(const MKLData&) = delete;
        MKLData& operator=(const MKLData&) = delete;
        MKLData() = default;
    };

    std::shared_ptr<MKLData> buildWeightMatrix() {
        auto data = std::make_shared<MKLData>();
        
        data->row_ptr.resize(n + 1);
        data->col_ind.reserve(n * k);
        data->values.reserve(n * k);

        data->row_ptr[0] = 0;
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<int, float>> row_data;
            row_data.reserve(k);
            
            for (int j = 0; j < k; ++j) {
                int neighbor = (*rkLists)[i * l + j];
                float weight = (float)(k - j);
                row_data.push_back({neighbor, weight});
            }
            
            std::sort(row_data.begin(), row_data.end());
            
            for (const auto& [col, val] : row_data) {
                data->col_ind.push_back(col);
                data->values.push_back(val);
            }
            
            data->row_ptr[i + 1] = data->col_ind.size();
        }

        MKL_CHECK(
            mkl_sparse_s_create_csr(&data->handle, SPARSE_INDEX_BASE_ZERO, n, n,
                                    data->row_ptr.data(), data->row_ptr.data() + 1,
                                    data->col_ind.data(), data->values.data()),
            "Failed to create weight matrix"
        );

        return data;
    }

    std::shared_ptr<MKLData> createTranspose(sparse_matrix_t& mat) {
        sparse_index_base_t indexing;
        MKL_INT rows, cols;
        MKL_INT *row_start, *row_end, *col_indx;
        float *vals;
        
        MKL_CHECK(
            mkl_sparse_s_export_csr(mat, &indexing, &rows, &cols,
                                    &row_start, &row_end, &col_indx, &vals),
            "Failed to export for transpose"
        );

        std::vector<MKL_INT> col_counts(cols, 0);
        for (MKL_INT i = 0; i < rows; ++i) {
            for (MKL_INT idx = row_start[i]; idx < row_end[i]; ++idx) {
                col_counts[col_indx[idx]]++;
            }
        }

        auto result_data = std::make_shared<MKLData>();
        result_data->row_ptr.resize(cols + 1);
        result_data->row_ptr[0] = 0;
        for (MKL_INT i = 0; i < cols; ++i) {
            result_data->row_ptr[i + 1] = result_data->row_ptr[i] + col_counts[i];
        }

        MKL_INT nnz = result_data->row_ptr[cols];
        result_data->col_ind.resize(nnz);
        result_data->values.resize(nnz);
        
        std::vector<MKL_INT> insert_pos = result_data->row_ptr;

        for (MKL_INT i = 0; i < rows; ++i) {
            for (MKL_INT idx = row_start[i]; idx < row_end[i]; ++idx) {
                MKL_INT col = col_indx[idx];
                MKL_INT pos = insert_pos[col]++;
                result_data->col_ind[pos] = i;
                result_data->values[pos] = vals[idx];
            }
        }

        MKL_CHECK(
            mkl_sparse_s_create_csr(&result_data->handle, SPARSE_INDEX_BASE_ZERO, cols, rows,
                                    result_data->row_ptr.data(), result_data->row_ptr.data() + 1,
                                    result_data->col_ind.data(), result_data->values.data()),
            "Failed to create transpose"
        );

        return result_data;
    }

    void updateRankedLists(sparse_matrix_t& mat) {
        sparse_index_base_t indexing;
        MKL_INT rows, cols;
        MKL_INT *row_start, *row_end, *col_indx;
        float *values;
        
        MKL_CHECK(
            mkl_sparse_s_export_csr(mat, &indexing, &rows, &cols,
                                    &row_start, &row_end, &col_indx, &values),
            "Failed to export matrix"
        );

        #pragma omp parallel for schedule(dynamic, 64)
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<float, int>> scores;
            scores.reserve(row_end[i] - row_start[i]);
            
            for (int idx = row_start[i]; idx < row_end[i]; ++idx) {
                scores.push_back({values[idx], col_indx[idx]});
            }

            std::sort(scores.begin(), scores.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

            int limit = std::min((int)scores.size(), (int)l);
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

    static MethodRegister<CprrMKL> signInMethod;
};

#endif