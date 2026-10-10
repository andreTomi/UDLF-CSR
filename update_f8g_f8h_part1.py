with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    content = f.read()

# Update F8g implementation preamble
old_f8g = '''inline void RFECore::f8g_rerank_cells() {
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
  f8g_matrix_.assign(nn * nn, 0.0f);  // zero-fill; NO diagonal pin
  f8g_inc_elems_.assign(nn, {});
  f8g_inc_elems_l_.assign(nn, {});
  f8g_pos_absent_ = 0;'''

new_f8g = '''inline void RFECore::f8g_rerank_cells() {
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
  f8g_pos_absent_ = 0;'''

if old_f8g in content:
    content = content.replace(old_f8g, new_f8g)
    print('F8g preamble updated')
else:
    print('F8g preamble NOT found')
    idx = content.find('f8g_matrix_.assign(nn * nn, 0.0f)')
    if idx != -1:
        print('Found at', idx)
        print(repr(content[idx:idx+200]))

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(content)