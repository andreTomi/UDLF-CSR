# Update the dense embycc matrix build section
old_embycc = '''  // Ensure dense embycc matrix is available for EXACT legacy compatibility.
  // The sparse Variant B diverges on diagonals due to missing structural zeros.
  // The dense Variant A uses the exact legacy algorithm and is bit-identical.
  // Build it on-demand from the sparse matrix (O(n * ncc), fine for gate fixtures n<=500).
  if (emb_by_cc_dense_.size() != nn * nn) {
    emb_by_cc_dense_.assign(nn * nn, 0.0f);  // Initialize ALL to 0.0f (structural zeros)
    // Build inverse mapping: dense CC index (0..ncc-1) -> original label
    std::vector<int> dense_to_original;
    dense_to_original.reserve(label_to_idx_.size());
    for (int orig = 0; orig < (int)label_to_idx_.size(); ++orig) {
      int dense = label_to_idx_[(std::size_t)orig];
      if (dense >= 0) {
        if ((int)dense_to_original.size() <= dense) dense_to_original.resize(dense + 1, -1);
        dense_to_original[dense] = orig;
      }
    }
    // Populate dense n x n matrix from sparse EmbCCMatrix (non-zero values only)
    // Zeros remain as structural zeros (matching reference's map which stores all CC labels)
    for (int q = 0; q < (int)n_; ++q) {
      int lo = emb_by_cc_sparse_.row_ptr[(std::size_t)q];
      int hi = emb_by_cc_sparse_.row_ptr[(std::size_t)q + 1];
      for (int idx = lo; idx < hi; ++idx) {
        int dense_ci = emb_by_cc_sparse_.col_idx[(std::size_t)idx];
        float val = emb_by_cc_sparse_.values[(std::size_t)idx];
        if (dense_ci >= 0 && dense_ci < (int)dense_to_original.size()) {
          int orig_label = dense_to_original[dense_ci];
          if (orig_label >= 0) {
            emb_by_cc_dense_[(std::size_t)q * nn + (std::size_t)orig_label] = val;
          }
        }
      }
    }
  }

  // Precompute non-zero column indices for each row (ascending order)
  // for two-pointer merge in dot product (matches reference's map intersection).
  std::vector<std::vector<int>> nnz_cols(nn);
  for (int q = 0; q < (int)n_; ++q) {
    const std::size_t base = (std::size_t)q * nn;
    for (std::size_t r = 0; r < nn; ++r) {
      if (emb_by_cc_dense_[base + r] != 0.0f) {
        nnz_cols[(std::size_t)q].push_back((int)r);
      }
    }
  }'''

new_embycc = '''  // Ensure dense embycc matrix is available for EXACT legacy compatibility.
  // The sparse Variant B diverges on diagonals due to missing structural zeros.
  // The dense Variant A uses the exact legacy algorithm and is bit-identical.
  // Build it on-demand from the sparse matrix (O(n * ncc), fine for gate fixtures n<=500).
  if (emb_by_cc_dense_.size() != nn * nn) {
    emb_by_cc_dense_.assign(nn * nn, 0.0f);  // Initialize ALL to 0.0f (structural zeros)
    // Build inverse mapping: dense CC index (0..ncc-1) -> original label
    std::vector<int> dense_to_original;
    dense_to_original.reserve(label_to_idx_.size());
    for (int orig = 0; orig < (int)label_to_idx_.size(); ++orig) {
      int dense = label_to_idx_[(std::size_t)orig];
      if (dense >= 0) {
        if ((int)dense_to_original.size() <= dense) dense_to_original.resize(dense + 1, -1);
        dense_to_original[dense] = orig;
      }
    }
    // Populate dense n x n matrix from sparse EmbCCMatrix (non-zero values only)
    // Zeros remain as structural zeros (matching reference's map which stores all CC labels)
    for (int q = 0; q < (int)n_; ++q) {
      int lo = emb_by_cc_sparse_.row_ptr[(std::size_t)q];
      int hi = emb_by_cc_sparse_.row_ptr[(std::size_t)q + 1];
      for (int idx = lo; idx < hi; ++idx) {
        int dense_ci = emb_by_cc_sparse_.col_idx[(std::size_t)idx];
        float val = emb_by_cc_sparse_.values[(std::size_t)idx];
        if (dense_ci >= 0 && dense_ci < (int)dense_to_original.size()) {
          int orig_label = dense_to_original[dense_ci];
          if (orig_label >= 0) {
            emb_by_cc_dense_[(std::size_t)q * nn + (std::size_t)orig_label] = val;
          }
        }
      }
    }
  }

  // Precompute non-zero column indices for each row (ascending order)
  // for two-pointer merge in dot product (matches reference's map intersection).
  std::vector<std::vector<int>> nnz_cols(nn);
  for (int q = 0; q < (int)n_; ++q) {
    const std::size_t base = (std::size_t)q * nn;
    for (std::size_t r = 0; r < nn; ++r) {
      if (emb_by_cc_dense_[base + r] != 0.0f) {
        nnz_cols[(std::size_t)q].push_back((int)r);
      }
    }
  }

  // Build inverse mapping dense_to_original for use in dot product (needed for CSR variant too)
  std::vector<int> dense_to_original;
  dense_to_original.reserve(label_to_idx_.size());
  for (int orig = 0; orig < (int)label_to_idx_.size(); ++orig) {
    int dense = label_to_idx_[(std::size_t)orig];
    if (dense >= 0) {
      if ((int)dense_to_original.size() <= dense) dense_to_original.resize(dense + 1, -1);
      dense_to_original[dense] = orig;
    }
  }

  // Count valid CC labels (ncc)
  int ncc = 0;
  for (int idx : label_to_idx_) if (idx >= 0) ++ncc;'''

if old_embycc in content:
    content = content.replace(old_embycc, new_embycc)
    print('Embycc section updated')
else:
    print('Embycc section NOT found')
    idx = content.find('Ensure dense embycc matrix is available')
    if idx != -1:
        print('Found at', idx)
        print(repr(content[idx:idx+200]))

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(content)