with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    content = f.read()

# Fix 1: Position index rebuild in f8g_rerank_cells
old1 = '''  if (inv_rows_.size() != (std::size_t)n_) build_position_index();'''
new1 = '''  // CRITICAL: rebuild position index on POST-F7b rank lists.
  // The F1 build_position_index() used pre-F7b rk_; F7b rewrites rk_ in place.
  // The reference's getPosition scans the live rank lists (post-F7b), so we must
  // rebuild the inverted index here to match. H9 proved the index matches the
  // legacy's linear scan WHEN built from the same lists.
  build_position_index();'''

if old1 in content:
    content = content.replace(old1, new1)
    print('Fix 1 applied')
else:
    print('Fix 1 NOT found')

# Fix 2: Add F8h declaration after f8g_rerank_cells declaration
old2 = '''  void f8g_rerank_cells();
};'''
new2 = '''  void f8g_rerank_cells();

  // F8h (Etapa 8): the TAIL of reRankingByCCs (ref:1012-1020). Mirrors
  // f7b_rerank_cprr_tail but uses f8g_matrix_ (no diagonal pin, unwritten = 0)
  // and f8g_inc_elems_/f8g_inc_elems_l_ built by f8g.
  void f8h_rerank_tail();
};'''

if old2 in content:
    content = content.replace(old2, new2)
    print('Fix 2 applied')
else:
    print('Fix 2 NOT found')

# Fix 3: Add F8h implementation before #endif
old3 = '''  }
}


#endif  // RFE_CORE_HPP'''
new3 = '''  }
}


// F8h (Etapa 8): the TAIL of reRankingByCCs -- mirrors f7b_rerank_cprr_tail but
// uses f8g_matrix_ (no diagonal pin, unwritten = 0) and f8g_inc_elems_/
// f8g_inc_elems_l_ built by f8g. See ref:1012-1020.
inline void RFECore::f8h_rerank_tail() {
  // Preconditions: f8g must have run and populated the matrix and sets.
  if (f8g_matrix_.empty() || f8g_inc_elems_.size() != (std::size_t)n_ ||
      f8g_inc_elems_l_.size() != (std::size_t)n_) {
    throw std::runtime_error(
        "RFE: f8h_rerank_tail called before f8g_rerank_cells (f8g state missing)");
  }

  std::vector<char> missing_self((std::size_t)n_, 0);
  #pragma omp parallel for schedule(static)
  for (int q = 0; q < (int)n_; ++q) {
    // 1. extras = set_difference(f8g_inc_elems_[q], f8g_inc_elems_l_[q]).
    //    Both are std::set<int> (ascending) -- linear merge.
    std::vector<int> extras;
    std::set_difference(
        f8g_inc_elems_[(std::size_t)q].begin(), f8g_inc_elems_[(std::size_t)q].end(),
        f8g_inc_elems_l_[(std::size_t)q].begin(), f8g_inc_elems_l_[(std::size_t)q].end(),
        std::back_inserter(extras));

    // 2. rkTmp = first l of rk_[q] + extras (extras already ascending by id).
    std::vector<int> rkTmp((std::size_t)l_ + extras.size());
    for (int i = 0; i < l_; ++i) {
      rkTmp[(std::size_t)i] = rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i];
    }
    std::copy(extras.begin(), extras.end(), rkTmp.begin() + l_);

    // 3. a[j] = f8g_matrix_[q * n_ + rkTmp[j]] (NO diagonal pin; unwritten = 0).
    std::vector<float> a(rkTmp.size());
    for (std::size_t j = 0; j < rkTmp.size(); ++j) {
      const int img = rkTmp[j];
      a[j] = (img >= 0 && img < (int)n_)
                 ? f8g_matrix_[(std::size_t)q * (std::size_t)n_ + (std::size_t)img]
                 : 0.0f;
    }

    // 4. Stable DESCENDING sort (shifts on strict `<` -> stable_sort with `>`).
    //    Ties keep rkTmp order (current list first, then extras ascending).
    std::stable_sort(rkTmp.begin(), rkTmp.end(),
                     [&](int u, int v) {
                       const std::size_t iu = (std::size_t)(&u - &rkTmp[0]);
                       const std::size_t iv = (std::size_t)(&v - &rkTmp[0]);
                       return a[iu] > a[iv];
                     });

    // 5. Swap self to position 0 (NOT move-to-front; ref:758-770).
    auto it = std::find(rkTmp.begin(), rkTmp.end(), q);
    if (it != rkTmp.end()) {
      std::iter_swap(rkTmp.begin(), it);
    } else {
      missing_self[(std::size_t)q] = 1;
    }

    // 6. Copy back only first l entries.
    for (int i = 0; i < l_; ++i) {
      rk_[(std::size_t)l_ * (std::size_t)q + (std::size_t)i] = rkTmp[(std::size_t)i];
    }
  }

  // Report missing self (should not happen if input was validated, but legacy pins it).
  for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
    if (missing_self[q]) {
      throw std::runtime_error("RFE: F8h self missing in row " + std::to_string(q) +
                               " -- legacy pins self during the tail sort");
    }
  }
}


#endif  // RFE_CORE_HPP'''

if old3 in content:
    content = content.replace(old3, new3)
    print('Fix 3 applied')
else:
    print('Fix 3 NOT found')
    idx = content.rfind('#endif  // RFE_CORE_HPP')
    if idx != -1:
        print('Context:', repr(content[idx-80:idx+50]))

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(content)
print('Done')