/**
 * @file LHRRMKL.hpp
 * @brief Intel MKL implementation of LHRR - CORRECTED VERSION.
 * 
 * CRITICAL FIXES from original:
 * 1. Properly accumulates MainMat across iterations (was missing!)
 * 2. Correctly implements symmetric position matrix initialization
 * 3. Uses Hadamard product (element-wise multiply) for refinement
 * 4. Matches the exact algorithm flow of LHRRSparse
 * 
 * This now provides a FAIR comparison to the CSR implementation.
 */

#ifndef LHRR_MKL_HPP
#define LHRR_MKL_HPP

#include "Method.hpp"
#include <mkl.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <unordered_map>

#ifndef MKL_CHECK
#define MKL_CHECK(call, msg) \
    do { \
        sparse_status_t status = (call); \
        if (status != SPARSE_STATUS_SUCCESS) { \
            std::cerr << "MKL Error: " << msg << " (line " << __LINE__ << ")" << std::endl; \
            if (status == SPARSE_STATUS_ALLOC_FAILED) { \
                std::cerr << "OUT OF MEMORY - Expected for LHRR N > 100K" << std::endl; \
            } \
            exit(1); \
        } \
    } while(0)
#endif

class LHRRMKL : public Method {
public:
    explicit LHRRMKL() : Method() {}

protected:
    void preprocess() override {
        l = config.get<int>("PARAM_LHRR_MKL_L");
        k = config.get<int>("PARAM_LHRR_MKL_K");
        t = config.get<int>("PARAM_LHRR_MKL_T");
        logK = std::log((float)k);

        dataProvider.setL(l);   
        
        logMessage("[LHRR-MKL] Initialized (Corrected Implementation)");
    }

    void execute() override {
        // Phase 1: Initialize Position Matrix (Symmetric)
        std::cout << "\n[LHRR-MKL] Phase 1: Initializing Position Matrix..." << std::endl;
        
        auto MainMat_wrapper = buildSymmetricPositionMatrix();
        sparse_matrix_t MainMat = MainMat_wrapper->handle; // Keep wrapper alive via shared_ptr if needed, but here we update it
        
        updateRankedLists(MainMat);

        // We need to keep MainMat wrapper alive across iterations
        auto currentMainMatWrapper = MainMat_wrapper;

        // Phase 2: Iterative Refinement
        for (int iter = 1; iter <= t; ++iter) {
            std::cout << "\n[LHRR-MKL] Iteration " << iter << "/" << t << std::endl;

            // A. Build Hyperedge Matrix H
            std::cout << "  Building hyperedge matrix H..." << std::endl;
            auto H_wrapper = buildHyperedgeMatrix();
            sparse_matrix_t H = H_wrapper->handle;

            // B. Transpose H
            std::cout << "  Computing H^T..." << std::endl;
            auto HT_wrapper = createTranspose(H);
            sparse_matrix_t H_T = HT_wrapper->handle;

            // C. Compute S = H * H^T
            std::cout << "  Computing S = H * H^T..." << std::endl;
            sparse_matrix_t S;
            MKL_CHECK(
                mkl_sparse_spmm(SPARSE_OPERATION_NON_TRANSPOSE, H, H_T, &S),
                "Failed S = H * H^T"
            );
            MKL_CHECK(mkl_sparse_order(S), "Failed to order S"); // CRITICAL: Ensure sorted usage for Hadamard

            // D. Compute S_rev = H^T * H
            std::cout << "  Computing S_rev = H^T * H..." << std::endl;
            sparse_matrix_t S_rev;
            MKL_CHECK(
                mkl_sparse_spmm(SPARSE_OPERATION_NON_TRANSPOSE, H_T, H, &S_rev),
                "Failed S_rev = H^T * H"
            );
            MKL_CHECK(mkl_sparse_order(S_rev), "Failed to order S_rev"); // CRITICAL: Ensure sorted usage for Hadamard

            // E. Hadamard Product: S_total = S ⊙ S_rev
            std::cout << "  Computing S_total = S ⊙ S_rev (Hadamard)..." << std::endl;
            auto Stotal_wrapper = hadamardProduct(S, S_rev);
            sparse_matrix_t S_total = Stotal_wrapper->handle;

            // F. CRITICAL: Accumulate into MainMat (Hadamard product)
            // MainMat = MainMat ⊙ S_total
            std::cout << "  Updating MainMat = MainMat ⊙ S_total..." << std::endl;
            
            // Ensure MainMat is ordered (it should be, but safety first)
            MKL_CHECK(mkl_sparse_order(currentMainMatWrapper->handle), "Failed to order MainMat");

            // Current MainMat is inside currentMainMatWrapper
            auto MainMatNew_wrapper = hadamardProduct(currentMainMatWrapper->handle, S_total);
            
            // Update current wrapper reference (old one dies if distinct)
            currentMainMatWrapper = MainMatNew_wrapper;
            
            // G. Update ranked lists
            updateRankedLists(currentMainMatWrapper->handle);

            // Cleanup iteration
            // H_wrapper, HT_wrapper, Stotal_wrapper go out of scope and destroy handles
            mkl_sparse_destroy(S);
            mkl_sparse_destroy(S_rev);
        }
        
        // currentMainMatWrapper goes out of scope at end of execute
    }

    void postprocess() override {
        logMessage("[LHRR-MKL] Completed");
    }

private:
    int l, k, t;
    float logK;

    struct MKLData {
        std::vector<MKL_INT> row_ptr;
        std::vector<MKL_INT> col_ind;
        std::vector<float> values;
        sparse_matrix_t handle = nullptr;

        ~MKLData() {
            if (handle) mkl_sparse_destroy(handle);
        }

        MKLData(const MKLData&) = delete;
        MKLData& operator=(const MKLData&) = delete;
        MKLData() = default;
    };

    // Build symmetric position matrix: P = W + W^T
    std::shared_ptr<MKLData> buildSymmetricPositionMatrix() {
        auto data = std::make_shared<MKLData>();
        
        data->row_ptr.resize(n + 1);
        
        // Use map to accumulate symmetric entries
        std::vector<std::unordered_map<int, float>> rows(n);

        // Forward and backward emission
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < l; ++j) {
                int neighbor = (*rkLists)[i * l + j];
                float w = (float)(l - j);
                
                // Forward: P[i][neighbor] += w
                rows[i][neighbor] += w;
                
                // Backward: P[neighbor][i] += w
                rows[neighbor][i] += w;
            }
        }

        // Convert to CSR
        data->row_ptr[0] = 0;
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<int, float>> row_data(rows[i].begin(), rows[i].end());
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
            "Failed to create position matrix"
        );

        return data;
    }

    // Build hyperedge matrix with logarithmic weighting
    std::shared_ptr<MKLData> buildHyperedgeMatrix() {
        auto data = std::make_shared<MKLData>();
        data->row_ptr.resize(n + 1);

        std::vector<std::unordered_map<int, float>> rows(n);

        // Build hyperedges: H[i][j] = sum over neighbors
        for (int i = 0; i < n; ++i) {
            // Self-loop
            rows[i][i] = 1.0f;

            // Two-hop neighborhood
            for (int o = 0; o < k; ++o) {
                int imgO = (*rkLists)[i * l + o];
                float wO = weightPosition(o + 1);

                for (int j = 0; j < k; ++j) {
                    int imgJ = (*rkLists)[imgO * l + j];
                    float wJ = weightPosition(j + 1);
                    
                    rows[i][imgJ] += wO * wJ;
                }
            }
        }

        // Convert to CSR
        data->row_ptr[0] = 0;
        for (int i = 0; i < n; ++i) {
            std::vector<std::pair<int, float>> row_data(rows[i].begin(), rows[i].end());
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
            "Failed to create hyperedge matrix"
        );

        return data;
    }

    // Explicit transpose (since MKL doesn't support A * B^T directly)
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

        // Count entries per column
        std::vector<MKL_INT> col_counts(cols, 0);
        for (MKL_INT i = 0; i < rows; ++i) {
            for (MKL_INT idx = row_start[i]; idx < row_end[i]; ++idx) {
                col_counts[col_indx[idx]]++;
            }
        }

        auto result_data = std::make_shared<MKLData>();
        result_data->row_ptr.resize(cols + 1);
        
        // Build transposed CSR
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

        // Sort each row
        std::vector<MKL_INT>& t_row_ptr = result_data->row_ptr;
        std::vector<MKL_INT>& t_col_idx = result_data->col_ind;
        std::vector<float>& t_vals = result_data->values;

        for (MKL_INT i = 0; i < cols; ++i) {
            MKL_INT start = t_row_ptr[i];
            MKL_INT end = t_row_ptr[i + 1];
            
            for (MKL_INT j = start; j < end - 1; ++j) {
                MKL_INT min_idx = j;
                for (MKL_INT k = j + 1; k < end; ++k) {
                    if (t_col_idx[k] < t_col_idx[min_idx]) {
                        min_idx = k;
                    }
                }
                if (min_idx != j) {
                    std::swap(t_col_idx[j], t_col_idx[min_idx]);
                    std::swap(t_vals[j], t_vals[min_idx]);
                }
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

    // Hadamard product: C = A ⊙ B (element-wise multiply)
    std::shared_ptr<MKLData> hadamardProduct(sparse_matrix_t& A, sparse_matrix_t& B) {
        // Export both matrices
        sparse_index_base_t idx_a, idx_b;
        MKL_INT rows_a, cols_a, rows_b, cols_b;
        MKL_INT *rs_a, *re_a, *ci_a, *rs_b, *re_b, *ci_b;
        float *v_a, *v_b;
        
        MKL_CHECK(
            mkl_sparse_s_export_csr(A, &idx_a, &rows_a, &cols_a, &rs_a, &re_a, &ci_a, &v_a),
            "Failed export A"
        );
        MKL_CHECK(
            mkl_sparse_s_export_csr(B, &idx_b, &rows_b, &cols_b, &rs_b, &re_b, &ci_b, &v_b),
            "Failed export B"
        );

        auto result_data = std::make_shared<MKLData>();
        result_data->row_ptr.resize(n + 1);
        
        // We can't pre-calculate size easily without running the merge, 
        // but we can use vectors to build and then copy to flat arrays
        std::vector<MKL_INT> col_ind_tmp;
        std::vector<float> values_tmp;

        result_data->row_ptr[0] = 0;
        for (int i = 0; i < n; ++i) {
            // Merge-like traversal of row i in both matrices
            MKL_INT idx_a = rs_a[i];
            MKL_INT idx_b = rs_b[i];
            MKL_INT end_a = re_a[i];
            MKL_INT end_b = re_b[i];

            while (idx_a < end_a && idx_b < end_b) {
                if (ci_a[idx_a] == ci_b[idx_b]) {
                    // Both have this column - multiply values
                    col_ind_tmp.push_back(ci_a[idx_a]);
                    values_tmp.push_back(v_a[idx_a] * v_b[idx_b]);
                    idx_a++;
                    idx_b++;
                } else if (ci_a[idx_a] < ci_b[idx_b]) {
                    idx_a++;
                } else {
                    idx_b++;
                }
            }
            
            result_data->row_ptr[i + 1] = col_ind_tmp.size();
        }

        // Move to persistent storage in wrapper
        result_data->col_ind = std::move(col_ind_tmp);
        result_data->values = std::move(values_tmp);

        MKL_CHECK(
            mkl_sparse_s_create_csr(&result_data->handle, SPARSE_INDEX_BASE_ZERO, n, n,
                                    result_data->row_ptr.data(), result_data->row_ptr.data() + 1,
                                    result_data->col_ind.data(), result_data->values.data()),
            "Failed to create Hadamard product"
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
            "Failed export for update"
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

            int limit = std::min((int)scores.size(), l);
            for (int j = 0; j < limit; ++j) {
                (*rkLists)[i * l + j] = scores[j].second;
            }
            
            // Self-correction
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
        return (pos == 1) ? 1.0f : (1.0f - std::log((float)pos) / logK);
    }

    static MethodRegister<LHRRMKL> signInMethod;
};

#endif