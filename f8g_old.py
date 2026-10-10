inline void RFECore::f8g_rerank_cells() {
  // Preconditions (same style as f8e->f8f, Etapa 6b item 2).
  if (label_to_idx_.size() != (std::size_t)n_ ||
      sorted_ccemb_.size() != (std::size_t)n_ ||
      ccemb_rows_.size() != (std::size_t)n_) {
    throw std::runtime_error(
        "RFE: f8g_rerank_cells called before f8e/f8f (label_to_idx_/sorted_ccemb_/"
        "ccemb_rows_ not sized N)");
  }
  if ((f8_embycc_sparse_ && emb_by_cc_sparse_.row_ptr.size() != (std::size_t)n_ + 1) ||
      (!f8_embycc_sparse_ && emb_by_cc_dense_.size() != (std::size_t)n_ * (std::size_t)n_)) {
    throw std::runtime_error(
        "RFE: f8g_rerank_cells called before f8f_compute_embs_by_ccs (embycc not built)");
  }
  // CRITICAL: rebuild position index on POST-F7b rank lists.
  // The F1 build_position_index() used pre-F7b rk_; F7b rewrites rk_ in place.
  // The reference's getPosition scans the live rank lists (post-F7b), so we must
  // rebuild the inverted index here to match. H9 proved the index matches the
  // legacy's linear scan WHEN built from the same lists.
  build_position_index();

  const std::size_t nn = (std::size_t)n_;
  f8g_inc_elems_.assign(nn, {});
  f8g_inc_elems_l_.assign(nn, {});
  f8g_pos_absent_ = 0;

  if (f8g_matrix_variant_ == F8gMatrixVariant::A_Dense) {
    f8g_matrix_dense_.assign(nn * nn, 0.0f);  // zero-fill; NO diagonal pin
  } else {
    // Variant B (CSR): collect triples per thread, then merge into CSR
    f8g_matrix_csr_ = F8gCSRMatrix();
    f8g_matrix_csr_.nrows = n_;
  }
  f8g_pos_absent_ = 0;

  // Ensure dense embycc matrix is available for EXACT legacy compatibility.
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
  for (int idx : label_to_idx_) if (idx >= 0) ++ncc;

  // For each CC label r (ascending), the top-member rows k1 = sorted_ccemb_[r][i]
  // it writes. We pair each k1 with its INDEX i, because the reference writes in
  // r-major order (r, then i, then j) -- so on a (k1,k2) write-conflict the LAST
  // write is the one with the LARGEST r (ties broken by larger i within that r).
  // Replaying in ascending (r, i) reproduces that last-writer exactly.
  std::vector<std::vector<std::pair<int, int>>> cc_for_k1(nn);  // (r, i) per k1
  for (int r = 0; r < (int)n_; ++r) {
    if (ccemb_rows_[(std::size_t)r].empty()) continue;  // no ccemb for this label
    const auto& sc = sorted_ccemb_[(std::size_t)r];
    const int lim = std::min(k_, (int)sc.size());  // == min(k, d.size()) per ref:991
    for (int i = 0; i < lim; ++i) {
      const int k1 = sc[(std::size_t)i];
      if (k1 < 0 || k1 >= (int)n_) continue;
      cc_for_k1[(std::size_t)k1].emplace_back(r, i);
    }
  }
  // Ascending (r, i): the last element is the last writer in r-major order.
  for (auto& vec : cc_for_k1)
    std::sort(vec.begin(), vec.end());

  long long absent = 0;
  #pragma omp parallel for schedule(dynamic) reduction(+:absent)
  for (int k1 = 0; k1 < (int)n_; ++k1) {
    for (const auto& ir : cc_for_k1[(std::size_t)k1]) {
      const int r = ir.first;
      const int i = ir.second;
      const auto& sc = sorted_ccemb_[(std::size_t)r];
      const int lim = std::min(k_, (int)sc.size());
      {
        for (int j = 0; j < lim; ++j) {
          const int k2 = sc[(std::size_t)j];
          if (k2 < 0 || k2 >= (int)n_) continue;
          // Dense dot product over ALL ncc CC label columns (not just non-zero).
          // The reference's simEmb iterates over map intersection which includes
          // structural zeros (0.0f * 0.0f = 0.0f). We iterate over all ncc CC labels
          // by counting valid entries in label_to_idx_ (where value >= 0).
          float sim = 0.0f;
          int ncc = 0;
          for (int idx : label_to_idx_) if (idx >= 0) ++ncc;
          for (int dense_ci = 0; dense_ci < ncc; ++dense_ci) {
            int orig_label = -1;
            // Find original label for this dense index
            for (int orig = 0; orig < (int)label_to_idx_.size(); ++orig) {
              if (label_to_idx_[(std::size_t)orig] == dense_ci) {
                orig_label = orig;
                break;
              }
            }
            if (orig_label < 0) continue;
            float v1 = emb_by_cc_dense_[(std::size_t)k1 * nn + (std::size_t)orig_label];
            float v2 = emb_by_cc_dense_[(std::size_t)k2 * nn + (std::size_t)orig_label];
            sim += v1 * v2;  // float accumulation matches reference's simEmb
          }
          const unsigned int pos = (unsigned int)position_of((idx_t)k1, (idx_t)k2);
          if (pos == (unsigned int)(l_ + 2)) ++absent;
          const double newSim =
              (1.0 + std::sqrt((double)(i + 1) * (double)(j + 1)) * sim) / (double)pos;
          f8g_matrix_dense_[nn * (std::size_t)k1 + (std::size_t)k2] = (float)newSim;
          f8g_inc_elems_[(std::size_t)k1].insert(k2);
        }
      }
    }
  }
  f8g_pos_absent_ = absent;

  // incElemsL_: the ids in each q's POST-f7b rank list (ref:1005-1010). Serial.
  for (int q = 0; q < (int)n_; ++q) {
    for (int i = 0; i < l_; ++i) {
      f8g_inc_elems_l_[(std::size_t)q].insert(
          rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i]);
    }
  }
}


// F8h (Etapa 8): the TAIL of reRankingByCCs -- mirrors f7b_rerank_cprr_tail but
// uses f8g_matrix_ (no diagonal pin, unwritten = 0) and f8g_inc_elems_/
// f8g_inc_elems_l_ built by f8g. See ref:1012-1020.
