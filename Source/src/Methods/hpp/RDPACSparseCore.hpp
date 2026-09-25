#ifndef RDPAC_SPARSE_CORE_HPP
#define RDPAC_SPARSE_CORE_HPP

#include "SparseMatrix.hpp"

#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// OpenMP, bit-exact sparse reformulation of the legacy RDPAC pipeline
// (repo-root RDPAC.cpp). The framework coupling lives in RDPAC.hpp; this
// header stays standalone so the parity gates (Source/tests) can run it
// against the frozen dense reference at any thread count. The iteration
// loop hoists W (15x -> 1x): W depends only on rk_, which is frozen while
// the k-loop runs, so the reference's per-iteration clean+generate+
// normalize of W is idempotent and runs once.
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
// double exactly as the reference expression does. OpenMP splits only
// row-independent work (or reader/writer phases separated by a barrier),
// so results are identical at any thread count -- enforced by the parity
// gate and the thread-determinism md5 stage in run_all.sh. Do NOT compile
// the parity gate with -ffast-math.
//
// Wait policy: on SMT hosts, run under OMP_WAIT_POLICY=PASSIVE. libgomp's
// default ACTIVE spin-wait in multiply_l1's per-level barriers starves the
// hardware thread each spinner shares a core with (bench_rdpac: 0.45x at
// 12 threads on 6 cores without it, 1.9-4.9x with it). The policy only
// changes the waiting strategy -- results are bit-exact under either
// (enforced by the gates, which run both ways).
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
        tmp_col_(n, 0.0f) {
    validate();
  }

  // Full pipeline in the exact order of run_reference_pipeline().
  // Hook names are stable and are what test_rdpac_parity compares.
  void run(Hook hook = nullptr) {
    fill_and_sort();
    build_inv();
    notify(hook, "post_fill");

    // W-hoist (15x -> 1x): W is a pure function of rk_ (frozen while the
    // loop runs) and p/k_end, so the dense loop's clean+generate+normalize
    // of W is idempotent. Run it once, in the reference's first-iteration
    // order: generate -> [dump w_k] -> copy raw W into RWL -> normalize
    // RWL -> [dump rwl_post_norm] -> normalize W.
    generate_w();
    notify(hook, "w_k");
    copy_w_to_rwl();
    normalize_rwl();
    notify(hook, "rwl_post_norm");
    normalize_w();

    bool first = true;
    for (long long k = k_start_; k <= k_end_;
         k += static_cast<long long>(k_inc_)) {
      // Upstream computes curK = K_END on every iteration (the loop variable
      // only selects first/last bookkeeping); mirror that quirk exactly.
      // Iterations after the first re-normalize RWL only: W already holds
      // the value the reference would regenerate (see the hoist above).
      if (!first) normalize_rwl();

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
      bool has_self = false;
      for (size_type pos = 0; pos < width_; ++pos) {
        const int col = rk_[index(row, pos)];
        if (col < 0 || static_cast<size_t>(col) >= n_) {
          throw std::invalid_argument("RDPAC rank id out of range");
        }
        if (seen[col] == static_cast<int>(row)) {
          throw std::invalid_argument("RDPAC duplicate id in ranked list row");
        }
        seen[col] = static_cast<int>(row);
        has_self = has_self || (col == static_cast<int>(row));
      }
      // Checked here (serially) so the parallel sorts never have to throw
      // from inside an OpenMP region; the legacy kernel would read one past
      // the row and swap a neighbouring row's first element instead.
      if (!has_self) {
        throw std::invalid_argument("RDPAC query missing from ranked list");
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
    // Row-block parallel emission, one allocator per thread. A cell receives
    // at most two records (direct + mirror, ids are unique per row), and
    // float addition is commutative, so the builder's (thread, emission)
    // ordering yields the same summed value as the dense source-ascending
    // accumulation regardless of the row partition.
    const int max_threads = omp_get_max_threads();
    std::vector<Mat::PaddedAllocator> allocators(max_threads);
    std::vector<std::pair<Mat::size_type, Mat::size_type>> ranges(max_threads);

    #pragma omp parallel
    {
      const int tid = omp_get_thread_num();
      const int nt = omp_get_num_threads();
      auto& allocator = *allocators[tid].ptr;
      // Partition with the ACTUAL team size so every row is covered even
      // when the runtime provides fewer threads than omp_get_max_threads().
      const size_type chunk =
          (n_ + static_cast<size_type>(nt) - 1) / static_cast<size_type>(nt);
      const size_type r_start =
          std::min(n_, static_cast<size_type>(tid) * chunk);
      const size_type r_end =
          std::min(n_, static_cast<size_type>(tid + 1) * chunk);
      ranges[tid] = {static_cast<Mat::size_type>(r_start),
                     static_cast<Mat::size_type>(r_end)};

      for (size_type source = r_start; source < r_end; ++source) {
        for (size_type pos = 0; pos < l_; ++pos) {
          const size_t col_i =
              static_cast<size_t>(rk_[index(source, pos)]);
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
    #pragma omp parallel for schedule(static)
    for (long long row_ll = 0; row_ll < static_cast<long long>(n_); ++row_ll) {
      const size_type row = static_cast<size_type>(row_ll);
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
  // scores/cols are caller-provided per-thread scratch (reused across rows).
  // All input invariants (id range, uniqueness, query presence) were checked
  // in validate(), so this cannot throw inside an OpenMP region.
  void sort_row(size_type row, bool use_prod, std::vector<float>& scores,
                std::vector<int>& cols) {
    scores.resize(width_);
    cols.resize(width_);
    for (size_type pos = 0; pos < width_; ++pos) {
      const int col = rk_[index(row, pos)];
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
    if (query == width_) return;  // unreachable: validate() checked presence
    if (query != 0) {
      std::swap(cols[0], cols[query]);
      std::swap(scores[0], scores[query]);
    }
    for (size_type pos = 0; pos < width_; ++pos) {
      rk_[index(row, pos)] = cols[pos];
    }
  }

  void sort_rows(bool use_prod) {
    #pragma omp parallel
    {
      std::vector<float> scores;
      std::vector<int> cols;
      #pragma omp for schedule(static)
      for (long long row_ll = 0; row_ll < static_cast<long long>(n_);
           ++row_ll) {
        sort_row(static_cast<size_type>(row_ll), use_prod, scores, cols);
      }
    }
  }

  // ---- Phase S2: W generation + normalizations ----------------------------
  // generatekNNMatrixW(curK): clean buffer, self=1 then pow overwrite at
  // rk positions 0..k_end-1. Position k_end stays 0 (snapshot slot).
  // Row-local writes: parallel safe.
  void generate_w() {
    #pragma omp parallel for schedule(static)
    for (long long row_ll = 0; row_ll < static_cast<long long>(n_); ++row_ll) {
      const size_type row = static_cast<size_type>(row_ll);
      for (size_type pos = 0; pos < static_cast<size_t>(k_end_); ++pos) {
        w_[w_index(row, pos)] = static_cast<float>(std::pow(p_, pos + 1));
      }
      w_[w_index(row, static_cast<size_t>(k_end_))] = 0.0f;
    }
  }

  // First iteration only: RWL := W restricted to the first L positions.
  // Positions >= k_end contribute W=0 (self sits at pos 0 after the S1 sort).
  void copy_w_to_rwl() {
    #pragma omp parallel
    {
      #pragma omp for schedule(static)
      for (long long idx = 0; idx < static_cast<long long>(rwl_.size());
           ++idx) {
        rwl_[static_cast<size_t>(idx)] = 0.0f;
      }
      #pragma omp for schedule(static)
      for (long long row_ll = 0; row_ll < static_cast<long long>(n_);
           ++row_ll) {
        const size_type row = static_cast<size_type>(row_ll);
        for (size_type pos = 0; pos < l_; ++pos) {
          const float v = (pos < static_cast<size_t>(k_end_))
                              ? w_[w_index(row, pos)]
                              : 0.0f;
          rwl_[index(row, pos)] = v;
        }
      }
    }
  }

  // normalizeMatrix(m, lim) -- float colsum, row-major, no zero guard
  // (0/0 and x/0 reproduce the reference's NaN/Inf verbatim).
  // The colsum accumulation stays SERIAL: its order (row asc, pos asc) is
  // part of the float contract. The division is cell-local -> parallel.
  void normalize_rwl() {
    std::vector<float> sums(n_, 0.0f);
    for (size_type row = 0; row < n_; ++row) {
      for (size_type pos = 0; pos < l_; ++pos) {
        sums[static_cast<size_t>(rk_[index(row, pos)])] +=
            rwl_[index(row, pos)];
      }
    }
    #pragma omp parallel for schedule(static)
    for (long long row_ll = 0; row_ll < static_cast<long long>(n_); ++row_ll) {
      const size_type row = static_cast<size_type>(row_ll);
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
    #pragma omp parallel for schedule(static)
    for (long long row_ll = 0; row_ll < static_cast<long long>(n_); ++row_ll) {
      const size_type row = static_cast<size_type>(row_ll);
      for (size_type pos = 0; pos < static_cast<size_t>(k_end_); ++pos) {
        const size_t col = static_cast<size_t>(rk_[index(row, pos)]);
        w_[w_index(row, pos)] /= sums[col];
      }
    }
  }

  // ---- Phase S3: masked diffusion (multiplyByTranspostL[StoreList]) -------
  // Rows are independent: row i reads only RWL row i and the read-only W,
  // and writes only RWL row i (and its own nonzero_/nz_pos_ lists). Dense
  // scratch becomes per-thread (dense_idx + tmp_line), so the computation
  // order per row is unchanged -> bit-exact at any thread count.
  void multiply_by_transpose(bool store_list) {
    nonzero_count_ = 0;
    #pragma omp parallel
    {
      std::vector<int32_t> dense_idx(n_, -1);
      std::vector<float> tmp_line(n_, 0.0f);
      #pragma omp for schedule(static)
      for (long long i_ll = 0; i_ll < static_cast<long long>(n_); ++i_ll) {
        const size_type i = static_cast<size_type>(i_ll);
        for (size_type pos = 0; pos < width_; ++pos) {
          dense_idx[static_cast<size_t>(rk_[index(i, pos)])] =
              static_cast<int32_t>(pos);
        }

        for (size_type j = 0; j < l_; ++j) {
          const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
          float acc = 0.0f;
          for (int lk = 0; lk < k_end_; ++lk) {
            const size_t jrow = jidx;  // W row = neighbor id
            const size_t lidx = static_cast<size_t>(
                rk_[index(jrow, static_cast<size_t>(lk))]);
            const int32_t ipos = dense_idx[lidx];
            const float rw =
                (ipos >= 0) ? rwl_[index(i, static_cast<size_t>(ipos))]
                            : 0.0f;
            const float wv = w_[w_index(jrow, static_cast<size_t>(lk))];
            acc += rw * wv;
          }
          tmp_line[jidx] = acc;
        }

        if (store_list) {
          nonzero_[i].clear();
          nz_pos_[i].clear();
          for (size_type j = 0; j < l_; ++j) {
            const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
            if (tmp_line[jidx] > 0.0f) {
              nonzero_[i].push_back(static_cast<uint32_t>(jidx));
              nz_pos_[i].push_back(static_cast<uint32_t>(j));
            }
          }
        }

        for (size_type j = 0; j < l_; ++j) {
          const size_t jidx = static_cast<size_t>(rk_[index(i, j)]);
          rwl_[index(i, j)] = tmp_line[jidx];
        }

        for (size_type pos = 0; pos < width_; ++pos) {
          dense_idx[static_cast<size_t>(rk_[index(i, pos)])] = -1;
        }
      }
    }
    if (store_list) {
      for (size_type i = 0; i < n_; ++i) {
        nonzero_count_ += nonzero_[i].size();
      }
    }
  }

  // sumIdentityAlpha: non-diagonal is float*alpha; the diagonal promotes to
  // double (1.0*(1.0-alpha)) exactly as the reference expression does.
  // Row-local writes -> parallel safe, order unchanged.
  void sum_identity_alpha(float alpha) {
    #pragma omp parallel for schedule(static)
    for (long long i_ll = 0; i_ll < static_cast<long long>(n_); ++i_ll) {
      const size_type i = static_cast<size_type>(i_ll);
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
  // in-place alias matrix<-matrix). For each jc the dense code computes every
  // tmpColumn[i] (phase 1, read-only w.r.t. dst) BEFORE writing any dst cell
  // (phase 2). The parallel version keeps that two-phase structure with a
  // barrier per jc, so the in-place dependency jc -> jc+1 is preserved and
  // each tmp_col_[i] keeps its exact nonzero_[i] accumulation order.
  // tmp_col_ is written once per i in phase 1 and read in phase 2 (barrier
  // provides visibility); per-i values are thread-independent.
  void multiply_l1(const std::vector<float>& m2) {
    #pragma omp parallel
    {
      for (size_type jc = 0; jc < width_; ++jc) {
        #pragma omp for schedule(static)
        for (long long i_ll = 0; i_ll < static_cast<long long>(n_); ++i_ll) {
          const size_type i = static_cast<size_type>(i_ll);
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

        #pragma omp for schedule(static)
        for (long long i_ll = 0; i_ll < static_cast<long long>(n_); ++i_ll) {
          const size_type i = static_cast<size_type>(i_ll);
          // dst(i, rk(i,jc)) is stored positionally at (i,jc).
          prod_[index(i, jc)] = tmp_col_[i];
        }
      }
    }
  }

  // copyingMatrixWToDBL: verbatim guard; products of nonnegative RWL are
  // >=0, so this normally is a no-op but is kept for parity. Cell-local.
  void copying_matrix_w_to_dbl() {
    #pragma omp parallel for schedule(static)
    for (long long i_ll = 0; i_ll < static_cast<long long>(n_); ++i_ll) {
      const size_type i = static_cast<size_type>(i_ll);
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
};

#endif  // RDPAC_SPARSE_CORE_HPP