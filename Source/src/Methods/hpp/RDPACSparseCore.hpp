#ifndef RDPAC_SPARSE_CORE_HPP
#define RDPAC_SPARSE_CORE_HPP

#include "SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Serial, bit-exact sparse reformulation of the legacy RDPAC pipeline
// (repo-root RDPAC.cpp). No OpenMP and no Method/framework coupling: the
// parity gate runs this standalone against Source/tests/test_rdpac_ref.cpp;
// execute() may call run() only after S1-S5 pass.
//
// Layout: rk_ is the framework's positional N x list_width ranked list.
// rwl_/prod_ are positional N x list_width buffers (column id is
// rk_[row*width + pos]); fill_ is the only CSR matrix, built once by
// build_from_partial_results and read by column during the initial sort.
// w_ is N x (k_end+1): positions 0..k_end-1 hold pow(p, pos+1), the extra
// slot exists only so the w_k snapshot can read position k_end (always 0).
// inv_ is a per-row (col,pos) index built after the fill sort; it replaces
// CSR lower_bound for the m2(l,j) lookups in multiplyL1.
//
// Bit-exactness contract: accumulation order mirrors the dense loops
// (lk asc, j asc, i asc) in float; sumIdentityAlpha's diagonal promotes to
// double exactly as the reference expression does. Do NOT compile the
// parity gate with -ffast-math.
class RDPACSparseCore {
 public:
  using size_type = std::size_t;
  using Mat = CSRMatrix<float>;
  using MatRow = Mat::size_type;
  using MatCol = Mat::index_type;
  using Hook = std::function<void(const char*)>;

  RDPACSparseCore(std::vector<int>& ranked_lists,
                  size_type n, size_type l, size_type list_width,
                  double p, double pl, int k_start, int k_inc, int k_end)
      : rk_(ranked_lists), n_(n), l_(l), width_(list_width),
        p_(p), pl_(pl), k_start_(k_start), k_inc_(k_inc), k_end_(k_end),
        w_(n * static_cast<size_t>(k_end > 0 ? k_end + 1 : 1), 0.0f),
        rwl_(n * list_width, 0.0f),
        prod_(n * list_width, 0.0f),
        nonzero_(n),
        nz_pos_(n),
        fill_(static_cast<MatRow>(n), static_cast<MatCol>(n), 2 * n * l),
        inv_(n),
        tmp_col_(n, 0.0f),
        tmp_line_(n, 0.0f),
        dense_idx_(n, -1) {
    validate();
  }

  // Full pipeline in the exact order of run_reference_pipeline().
  // Hook names are stable and are what test_rdpac_parity compares.
  void run(Hook hook = nullptr) {
    fill_and_sort();
    build_inv();
    notify(hook, "post_fill");

    bool first = true;
    for (long long k = k_start_; k <= k_end_;
         k += static_cast<long long>(k_inc_)) {
      // Upstream computes curK = K_END on every iteration (the loop variable
      // only selects first/last bookkeeping); mirror that quirk exactly.
      generate_w();
      if (first) notify(hook, "w_k");
      if (first) copy_w_to_rwl();

      normalize_rwl();
      if (first) notify(hook, "rwl_post_norm");
      normalize_w();

      const bool store = (k == static_cast<long long>(k_end_));
      multiply_by_transpose(store);

      sum_identity_alpha(0.95f);
      if (first) notify(hook, "post_iter0");
      first = false;
    }
    notify(hook, "post_15iter");

    normalize_rwl();
    multiply_l1(rwl_);   // RWL x RWL -> prod_ (dense: dst = matrix)
    notify(hook, "post_mult1");
    multiply_l1(prod_);  // RWL x prod_ -> prod_ (dense in-place alias)
    notify(hook, "post_mult2");

    copying_matrix_w_to_dbl();
    sort_rows(true);
    notify(hook, "final");
  }


  size_type n() const { return n_; }
  size_type l() const { return l_; }
  size_type width() const { return width_; }
  int k_start() const { return k_start_; }
  int k_end() const { return k_end_; }
  size_type fill_nnz() const { return fill_.nnz(); }
  size_type nonzero_count() const { return nonzero_count_; }

  int rk(size_type row, size_type pos) const { return rk_[index(row, pos)]; }
  float w(size_type row, size_type pos) const {
    if (pos > static_cast<size_type>(k_end_)) {
      throw std::out_of_range("RDPAC w() position past k_end");
    }
    return w_[w_index(row, pos)];
  }
  float rwl(size_type row, size_type pos) const { return rwl_[index(row, pos)]; }
  float prod(size_type row, size_type pos) const {
    return prod_[index(row, pos)];
  }
  float fill(size_type row, size_type col) const {
    return fill_.get(static_cast<MatRow>(row), static_cast<MatCol>(col));
  }
  const std::vector<uint32_t>& nonzero(size_type row) const {
    return nonzero_[row];
  }

 private:
  static size_type checked_mul(size_type a, size_type b, const char* what) {
    if (b != 0 && a > std::numeric_limits<size_type>::max() / b) {
      throw std::overflow_error(std::string("RDPAC overflow: ") + what);
    }
    return a * b;
  }

  void validate() {
    if (n_ == 0 || l_ == 0 || width_ < l_ || k_end_ <= 0 ||
        k_start_ < 1 || k_inc_ <= 0 || k_end_ > static_cast<int>(width_) ||
        k_start_ > k_end_) {
      throw std::invalid_argument("Invalid RDPAC dimensions/parameters");
    }
    if (rk_.size() != checked_mul(n_, width_, "ranked-list storage")) {
      throw std::invalid_argument("RDPAC ranked-list storage has wrong width");
    }
    if (!std::isfinite(p_) || !std::isfinite(pl_) || p_ < 0.0 || pl_ < 0.0) {
      throw std::invalid_argument("RDPAC p/pl must be finite and non-negative");
    }
    // Positional-layout invariant: one slot per column per row. With unique
    // ids, rk_[row*width+pos] IS the dense cell (row, col) -- required for
    // inv_/dense_idx_ lookups and every positional copy to equal the dense
    // by-column semantics (duplicates would make the dense cell a single
    // shared slot, e.g. sumIdentityAlpha would apply alpha twice to it).
    if (width_ > n_) {
      throw std::invalid_argument(
          "RDPAC list width exceeds N (ids must be unique per row)");
    }
    std::vector<int> seen(n_, -1);
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < width_; ++pos) {
        const int col = rk_[index(row, pos)];
        if (col < 0 || static_cast<size_t>(col) >= n_) {
          throw std::invalid_argument("RDPAC rank id out of range");
        }
        if (seen[col] == static_cast<int>(row)) {
          throw std::invalid_argument("RDPAC duplicate id in ranked list row");
        }
        seen[col] = static_cast<int>(row);
      }
    }
  }

  static void notify(const Hook& hook, const char* phase) {
    if (hook) hook(phase);
  }

  size_t index(size_t row, size_t pos) const { return row * width_ + pos; }
  size_t w_index(size_t row, size_t pos) const {
    return row * static_cast<size_t>(k_end_ + 1) + pos;
  }

  // ---- Phase S1: mirrored fill + first ranked-list sort --------------------
  // Emission order is source asc, pos asc, direct then mirror (one allocator,
  // serial). Builder V4 may reorder the two duplicate (row,col) records, but
  // a two-term float sum is commutative, so the cell is bit-identical to the
  // dense source-ascending accumulation for any input with <=2 contributions
  // per cell (true when each row's first L ids are unique).
  void fill_and_sort() {
    std::vector<Mat::PaddedAllocator> allocators(1);
    std::vector<std::pair<Mat::size_type, Mat::size_type>> ranges(
        1, {0, static_cast<Mat::size_type>(n_)});
    auto& allocator = *allocators[0].ptr;

    for (size_type source = 0; source < n_; ++source) {
      for (size_type pos = 0; pos < l_; ++pos) {
        const int col = rk_[index(source, pos)];
        if (col < 0 || static_cast<size_t>(col) >= n_) {
          throw std::invalid_argument("RDPAC rank id out of range");
        }
        const size_t col_i = static_cast<size_t>(col);
        const float value = static_cast<float>(std::pow(pl_, pos + 1));
        auto* direct = allocator.allocate();
        direct->row = static_cast<MatRow>(source);
        direct->col = static_cast<MatCol>(col_i);
        direct->value = value;
        direct->is_set = false;
        auto* mirror = allocator.allocate();
        mirror->row = static_cast<MatCol>(col_i);
        mirror->col = static_cast<MatCol>(source);
        mirror->value = value;
        mirror->is_set = false;
      }
    }
    Mat::build_from_partial_results(fill_, allocators, ranges);
    // copyingMatrixWToDBL after the fill is a no-op under zero-init (all
    // pl-pow terms are >= 0), so nothing is replicated here.
    sort_rows(false);
  }

  // Per-row inverse index (col -> position) for multiplyL1's m2(l,j) and
  // multiplyByTranspostL's RWL(i,lidx) lookups. Rows are unique-column by
  // construction (ranked lists), so lower_bound on this array is exact.
  void build_inv() {
    for (size_type row = 0; row < n_; ++row) {
      auto& entries = inv_[row];
      entries.clear();
      entries.reserve(width_);
      for (size_type pos = 0; pos < width_; ++pos) {
        entries.emplace_back(rk_[index(row, pos)],
                             static_cast<uint32_t>(pos));
      }
      std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
    }
  }

  // Position of column col in row, or -1 when absent.
  int find_pos(size_type row, int col) const {
    const auto& entries = inv_[row];
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), col,
        [](const std::pair<int, uint32_t>& e, int c) { return e.first < c; });
    if (it == entries.end() || it->first != col) return -1;
    return static_cast<int>(it->second);
  }

  // Dense kernelSortRankedLists: insertion sort (stable, strict <) over the
  // score array, then swap the query id (first hit) into position 0.
  // use_prod selects the score source: fill_ (S1) or prod_ (final sort).
  void sort_row(size_type row, bool use_prod) {
    std::vector<float> scores(width_);
    std::vector<int> cols(width_);
    for (size_type pos = 0; pos < width_; ++pos) {
      const int col = rk_[index(row, pos)];
      if (col < 0 || static_cast<size_t>(col) >= n_) {
        throw std::invalid_argument("RDPAC rank id out of range during sort");
      }
      cols[pos] = col;
      scores[pos] = use_prod
                        ? prod_[index(row, pos)]
                        : fill_.get(static_cast<MatRow>(row),
                                    static_cast<MatCol>(col));
    }

    for (size_t j = 1; j < width_; ++j) {
      const float key_a = scores[j];
      const int key_r = cols[j];
      size_t i = j;
      while (i > 0 && scores[i - 1] < key_a) {
        scores[i] = scores[i - 1];
        cols[i] = cols[i - 1];
        --i;
      }
      scores[i] = key_a;
      cols[i] = key_r;
    }

    size_t query = 0;
    while (query < width_ && cols[query] != static_cast<int>(row)) ++query;
    if (query == width_) {
      throw std::invalid_argument("RDPAC query missing from ranked list");
    }
    if (query != 0) {
      std::swap(cols[0], cols[query]);
      std::swap(scores[0], scores[query]);
    }
    for (size_type pos = 0; pos < width_; ++pos) {
      rk_[index(row, pos)] = cols[pos];
    }
  }

  void sort_rows(bool use_prod) {
    for (size_type row = 0; row < n_; ++row) sort_row(row, use_prod);
  }

  // ---- Phase S2: W generation + normalizations ----------------------------
  // generatekNNMatrixW(curK): clean buffer, self=1 then pow overwrite at
  // rk positions 0..k_end-1. Position k_end stays 0 (snapshot slot).
  void generate_w() {
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < static_cast<size_t>(k_end_); ++pos) {
        w_[w_index(row, pos)] = static_cast<float>(std::pow(p_, pos + 1));
      }
      w_[w_index(row, static_cast<size_t>(k_end_))] = 0.0f;
    }
  }

  // First iteration only: RWL := W restricted to the first L positions.
  // Positions >= k_end contribute W=0 (self sits at pos 0 after the S1 sort).
  void copy_w_to_rwl() {
    std::fill(rwl_.begin(), rwl_.end(), 0.0f);
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < l_; ++pos) {
        const float v = (pos < static_cast<size_t>(k_end_))
                            ? w_[w_index(row, pos)]
                            : 0.0f;
        rwl_[index(row, pos)] = v;
      }
    }
  }

  // normalizeMatrix(m, lim) -- float colsum, row-major, no zero guard
  // (0/0 and x/0 reproduce the reference's NaN/Inf verbatim).
  void normalize_rwl() {
    std::vector<float> sums(n_, 0.0f);
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < l_; ++pos) {
        sums[static_cast<size_t>(rk_[index(row, pos)])] +=
            rwl_[index(row, pos)];
      }
    }
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < l_; ++pos) {
        const size_t col = static_cast<size_t>(rk_[index(row, pos)]);
        rwl_[index(row, pos)] /= sums[col];
      }
    }
  }

  void normalize_w() {
    std::vector<float> sums(n_, 0.0f);
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < static_cast<size_t>(k_end_); ++pos) {
        sums[static_cast<size_t>(rk_[index(row, pos)])] +=
            w_[w_index(row, pos)];
      }
    }
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < static_cast<size_t>(k_end_); ++pos) {
        const size_t col = static_cast<size_t>(rk_[index(row, pos)]);
        w_[w_index(row, pos)] /= sums[col];
      }
    }
  }

  // ---- Phase S3: masked diffusion (multiplyByTranspostL[StoreList]) -------
  // All j < L tmpLine cells are computed before any RWL write, matching the
  // two-loop dense kernel. dense_idx_ gives O(1) RWL(i,lidx) lookups and is
  // cleared per row, so the scratch never grows to N^2.
  void multiply_by_transpose(bool store_list) {
    if (tmp_line_.size() < n_) tmp_line_.assign(n_, 0.0f);
    nonzero_count_ = 0;
    for (size_type i = 0; i < n_; ++i) {
      for (size_type pos = 0; pos < width_; ++pos) {
        dense_idx_[static_cast<size_t>(rk_[index(i, pos)])] =
            static_cast<int32_t>(pos);
      }

      for (size_type j = 0; j < l_; ++j) {
        const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
        float acc = 0.0f;
        for (int lk = 0; lk < k_end_; ++lk) {
          const size_t jrow = jidx;  // W row = neighbor id
          const size_t lidx =
              static_cast<size_t>(rk_[index(jrow, static_cast<size_t>(lk))]);
          const int32_t ipos = dense_idx_[lidx];
          const float rw = (ipos >= 0)
                               ? rwl_[index(i, static_cast<size_t>(ipos))]
                               : 0.0f;
          const float wv = w_[w_index(jrow, static_cast<size_t>(lk))];
          acc += rw * wv;
        }
        tmp_line_[jidx] = acc;
      }

      if (store_list) {
        nonzero_[i].clear();
        nz_pos_[i].clear();
        for (size_type j = 0; j < l_; ++j) {
          const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
          if (tmp_line_[jidx] > 0.0f) {
            nonzero_[i].push_back(static_cast<uint32_t>(jidx));
            nz_pos_[i].push_back(static_cast<uint32_t>(j));
            ++nonzero_count_;
          }
        }
      }

      for (size_type j = 0; j < l_; ++j) {
        const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
        rwl_[index(i, j)] = tmp_line_[jidx];
      }

      for (size_type pos = 0; pos < width_; ++pos) {
        dense_idx_[static_cast<size_t>(rk_[index(i, pos)])] = -1;
      }
    }
  }

  // sumIdentityAlpha: non-diagonal is float*alpha; the diagonal promotes to
  // double (1.0*(1.0-alpha)) exactly as the reference expression does.
  void sum_identity_alpha(float alpha) {
    for (size_type i = 0; i < n_; ++i) {
      for (size_type jc = 0; jc < width_; ++jc) {
        const int j = rk_[index(i, jc)];
        float& cell = rwl_[index(i, jc)];
        if (static_cast<size_t>(j) != i) {
          cell *= alpha;
        } else {
          const float lhs = cell * alpha;
          const double rhs = 1.0 * (1.0 - static_cast<double>(alpha));
          cell = static_cast<float>(lhs + rhs);
        }
      }
    }
  }

  // ---- Phase S5: multiplyL1 (two calls) ------------------------------------
  // m1 is always RWL; m2 is RWL (first call) or prod_ (second call, dense
  // in-place alias matrix<-matrix). tmp_col_ is the n-float tmpColumn buffer;
  // for each jc all rows are computed before any dst write, exactly as dense.
  void multiply_l1(const std::vector<float>& m2) {
    for (size_type jc = 0; jc < width_; ++jc) {
      for (size_type i = 0; i < n_; ++i) {
        const int j = rk_[index(i, jc)];
        float acc = 0.0f;
        for (size_t idx = 0; idx < nonzero_[i].size(); ++idx) {
          const size_t l = nonzero_[i][idx];
          // m1(i, l): position recorded at store time (row i, column l).
          const float v1 = rwl_[index(i, nz_pos_[i][idx])];
          // m2(l, j): row l, column j -- inverse index (or 0 when the
          // column is outside row l's list, matching the dense zero cell).
          const int pj = find_pos(l, j);
          const float v2 =
              (pj >= 0) ? m2[index(l, static_cast<size_t>(pj))] : 0.0f;
          acc += v1 * v2;
        }
        tmp_col_[i] = acc;
      }
      for (size_type i = 0; i < n_; ++i) {
        // dst(i, rk(i,jc)) is stored positionally at (i,jc).
        prod_[index(i, jc)] = tmp_col_[i];
      }
    }
  }

  // copyingMatrixWToDBL: verbatim guard; products of nonnegative RWL are
  // >=0, so this normally is a no-op but is kept for parity.
  void copying_matrix_w_to_dbl() {
    for (size_type i = 0; i < n_; ++i) {
      for (size_type pos = 0; pos < width_; ++pos) {
        float& cell = prod_[index(i, pos)];
        if (cell < 0.0f) {
          cell = static_cast<float>(static_cast<long>(n_) -
                                    static_cast<long>(pos));
        }
      }
    }
  }

  // Full pipeline, exact order of run_reference_pipeline() -- see the public
  // run() above (this comment anchors the phase-kernel section).




  std::vector<int>& rk_;
  size_type n_, l_, width_;
  double p_, pl_;
  int k_start_, k_inc_, k_end_;
  std::vector<float> w_, rwl_, prod_;
  std::vector<std::vector<uint32_t>> nonzero_;
  std::vector<std::vector<uint32_t>> nz_pos_;
  size_type nonzero_count_{0};
  Mat fill_;
  std::vector<std::vector<std::pair<int, uint32_t>>> inv_;
  std::vector<float> tmp_col_;
  std::vector<float> tmp_line_;
  std::vector<int32_t> dense_idx_;
};

#endif  // RDPAC_SPARSE_CORE_HPP