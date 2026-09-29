/**
 * @file BFSTreeCore.hpp
 * @brief BFSTree (rank diffusion over a similarity matrix W) on the CSRMatrix
 *        engine -- carrier A4 (exact-blocked F5, legacy-exact F6, fused-exact
 *        F7), bit-exact with the legacy F1..F9.
 *
 * Provenance (do not paraphrase): this core is a mechanical port of the carrier
 * the measurement campaign validated in Source/bench/bench_bfstree.cpp
 * (`run_A3(kern=1)` -> compute_w_diffusion_a4), which in turn is bit-exact
 * against the verbatim legacy embedded there. The verbatim reference lives in
 * Source/tests/bfstree_ref_verbatim.hpp for the parity gate, and the golden
 * CK_FINAL pin is the one Source/tests/gate_bfstree.sh enforces.
 *
 * Bit-exactness is by CONSTRUCTION, not by sampling:
 *   * every sum keeps the legacy accumulation order (F5 replays q's terms in
 *     the legacy sequence; F7 accumulates over posM ascending);
 *   * the only narrowing is the legacy `float(double / .)` division;
 *   * W >= 0 (rbo >= 0, diagonal = 1), so no cancellation exists anywhere.
 *
 * BUILD CONTRACT: the identity guarantees hold under the STRICT build (no
 * -ffast-math, no FMA contraction). Under the fast-math product build ~89% of W
 * cells differ by 1 ULP, which flips the downstream prune frontier and changes
 * ~81% of the final rank lists (measured; dossie numerics_prod, "Passo 0").
 * Source/src/Methods/hpp/BFSTree.hpp refuses to run otherwise.
 */
#ifndef BFSTREE_CORE_HPP
#define BFSTREE_CORE_HPP

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class BFSTreeCore {
 public:
  using M = CSRMatrix<float>;
  using idx_t = M::size_type;
  using Triplet = M::Triplet;

  // rk: row-major rank lists, n*l ints (legacy stride l). Read AND written in
  // place (F9 sorts them). B: batch of rows for F5/F7.
  BFSTreeCore(std::vector<int>& rk, size_t n, size_t l, int k, long B)
      : rk_(rk), n_((long)n), l_((int)l), topK_(k), B_(B < 1 ? 1 : B) {
    if (l_ < topK_) throw std::runtime_error("BFSTree: need L >= k");
    if (rk_.size() != (size_t)n * (size_t)l) {
      throw std::runtime_error(
          "BFSTree: rkLists size != N*L (check the input width vs PARAM_BFSTREE_L)");
    }
    if (B_ > n_) B_ = n_;
    matrix_.assign((size_t)n_ * (size_t)n_, 0.0f);   // F1/F2 scratch + raw W
  }

  // --------------------------------------------------------------- F1..F4 --
  // Verbatim semantics: normalize (+(l-pos) both directions), minZero, sort
  // with the pin (self first), correlation via rbo over the first topK.
  void front() {
    zero_matrix();
    compute_rank_normalization(1);
    compute_rank_normalization(2);
    compute_rank_correlation();
    zero_matrix();   // F4: the normalization matrix is scratch; W resets
  }

  // ------------------------------------------------------------------- F5 --
  // Exact-blocked accumulation: same term sequence per q as the legacy, same
  // per-cell accumulation order, replayed from the inverted index.
  void f5() {
    build_inv();
    const long n = n_;
    std::vector<float> scratch((size_t)B_ * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B_) {
      const long r1 = std::min<long>(n, r0 + B_);
      #pragma omp parallel for schedule(static)
      for (long a = r0; a < r1; a++) {
        float* rowbuf = &scratch[(size_t)(a - r0) * (size_t)n];
        std::fill(rowbuf, rowbuf + n, 0.0f);
        for (uint32_t q : inv_[(size_t)a])
          replay_q_into_row((int)q, (int)a, rowbuf);
        std::memcpy(&matrix_[(size_t)a * (size_t)n], rowbuf,
                    (size_t)n * sizeof(float));
      }
    }
    nnz_w_ = 0;
    for (size_t i = 0; i < matrix_.size(); ++i)
      if (matrix_[i] != 0.0f) ++nnz_w_;
  }

  // ------------------------------------------------------------------- F6 --
  // Legacy-exact column sums: start at 1.0, per column with rows ascending,
  // double accumulation (the only dense pass that survives).
  void f6() {
    const int n = (int)n_;
    colsum_.assign((size_t)n, 1.0);
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
      double a = 1.0;
      for (int j = 0; j < n; j++)
        a += (double)matrix_[(size_t)j * (size_t)n + (size_t)i];
      colsum_[(size_t)i] = a;
    }
  }

// ------------------------------------------------------------------- F7 --
  // Fused-exact kernel (A4): row-i hoist + ascending-column gather; every rc
  // cell bit-identical to the legacy dot product.
  void f7() {
    const int n = (int)n_, l = l_;
    const std::vector<double>& ac = colsum_;
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)n * (size_t)l);
    std::vector<float> batch((size_t)B_ * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B_) {
      const long r1 = std::min<long>(n, r0 + B_);
      #pragma omp parallel
      {
        std::vector<std::tuple<idx_t, idx_t, float>> local;
        std::vector<float> vi((size_t)l), buf((size_t)l);
        std::vector<int> ord((size_t)l);
        #pragma omp for schedule(static)
        for (long imgI = r0; imgI < r1; imgI++) {
          float* rbuf = &batch[(size_t)(imgI - r0) * (size_t)n];
          std::fill(rbuf, rbuf + n, 0.0f);
          const int* rki = &rk_[(size_t)imgI * (size_t)l];
          for (int p = 0; p < l; p++) {          // 1. hoisted row-i values
            const int c = rki[p];
            vi[(size_t)p] =
                (float)((double)matrix_[(size_t)imgI * (size_t)n + (size_t)c] /
                        ac[(size_t)c]);
          }
          for (int p = 0; p < l; p++) ord[(size_t)p] = p;
          std::sort(ord.begin(), ord.end(),
                    [&](int a, int b) { return rki[a] < rki[b]; });
          for (int posJ = 0; posJ < l; posJ++) {
            const int imgJ = rki[posJ];
            const float* wj = &matrix_[(size_t)imgJ * (size_t)n];
            for (int t = 0; t < l; t++) {        // 2. ascending-column gather
              const int p = ord[(size_t)t];
              const int c = rki[p];
              buf[(size_t)p] = (float)((double)wj[c] / ac[(size_t)c]);
            }
            float acc = 0.0f;
            for (int p = 0; p < l; p++) acc += vi[(size_t)p] * buf[(size_t)p];
            rbuf[imgJ] = acc;
          }
          for (int posJ = 0; posJ < l; posJ++)
            local.emplace_back((idx_t)imgI, (idx_t)rki[posJ], rbuf[rki[posJ]]);
        }
        #pragma omp critical
        { trips.insert(trips.end(), local.begin(), local.end()); }
      }
    }
    rc_ = build_csr((idx_t)n, (idx_t)n, trips);
  }

  // ---------------------------------------------------------------- F8/F9 --
  void f8() { rc8_ = normalize_w_prob_sparse(rc_); }
  void f9() {
    for (int i = 0; i < (int)n_; i++) rankingSort_sparse(i, rc8_);
  }

  void run() {
    front();
    f5();
    f6();
    f7();
    f8();
    f9();
  }

  // ------------------------------------------------------------- accessors --
  const std::vector<float>& w_raw() const { return matrix_; }     // F5 output
  const std::vector<double>& colsum() const { return colsum_; }   // F6 output
  const M& rc() const { return rc_; }                             // F7 output
  long nnz_w() const { return nnz_w_; }
  size_t dense_bytes() const { return matrix_.size() * sizeof(float); }
  static size_t csr_bytes(const M& m) {
    return (size_t)m.nnz() * (sizeof(idx_t) + sizeof(float)) +
           ((size_t)m.rows() + 1) * sizeof(idx_t);
  }

  // FNV-1a over raw bits -- deterministic digests for the gates.
  uint64_t ck_w() const {
    return fnv_bytes(matrix_.data(), matrix_.size() * sizeof(float));
  }
  uint64_t ck_rk() const {
    return fnv_bytes(rk_.data(), rk_.size() * sizeof(int));
  }
  uint64_t ck_csr(const M& m) const {
    uint64_t h = 14695981039346656037ULL;
    for (idx_t r = 0; r < (idx_t)n_; ++r) {
      m.for_each_in_row(r, [&](idx_t c, float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        uint64_t x = ((uint64_t)r << 32) ^ (uint64_t)c;
        h ^= x; h *= 1099511628211ULL;
        h ^= b; h *= 1099511628211ULL;
      });
    }
    return h;
  }

private:
  static uint64_t fnv_bytes(const void* p, size_t nbytes) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < nbytes; ++i) {
      h ^= b[i];
      h *= 1099511628211ULL;
    }
    return h;
  }

  // ---- F1/F2 verbatim (legacy BFSTree.cpp) --------------------------------
  void zero_matrix() { std::fill(matrix_.begin(), matrix_.end(), 0.0f); }
  inline int getRKElem(long query, long pos) const {
    return rk_[(size_t)l_ * (size_t)query + (size_t)pos];
  }
  inline float getMatrixElem(size_t i, size_t j) const {
    return matrix_[(size_t)n_ * i + j];
  }
  inline void setMatrixElem(size_t i, size_t j, float v) {
    matrix_[(size_t)n_ * i + j] = v;
  }
  static inline float minZero(float x, float y) {
    if (x == 0) return y;
    return std::min(x, y);
  }

  // Verbatim insertion sort with the pin (self moved to position 0).
  void rankingSort(int rk) {
    const int l = l_;
    const long LcurRL = ((long)l) * rk;
    std::vector<float> a((size_t)l);
    long index;
    for (int j = 0; j < l; j++) {
      index = (long)n_ * rk + rk_[LcurRL + j];
      a[(size_t)j] = matrix_[(size_t)index];
    }
    int i, j, keyR;
    float keyA;
    for (j = 1; j < l; j++) {
      keyA = a[(size_t)j];
      keyR = rk_[LcurRL + j];
      i = j - 1;
      while (i >= 0 && a[(size_t)i] < keyA) {
        a[(size_t)i + 1] = a[(size_t)i];
        rk_[LcurRL + i + 1] = rk_[LcurRL + i];
        i--;
      }
      a[(size_t)i + 1] = keyA;
      rk_[LcurRL + i + 1] = keyR;
    }
    i = 0;
    while ((rk_[LcurRL + i] != rk) && (i < l)) i++;
    if (i > 0) {
      int aux = rk_[LcurRL + 0];
      rk_[LcurRL + 0] = rk_[LcurRL + i];
      rk_[LcurRL + i] = aux;
      float auxA = a[0];
      a[0] = a[(size_t)i];
      a[(size_t)i] = auxA;
    }
  }
  void exec_sort_ranked_lists() {
    for (int i = 0; i < (int)n_; i++) rankingSort(i);
  }

  void compute_rank_normalization(int type) {
    switch (type) {
      case 1:
        for (long q = 0; q < n_; q++)
          for (int pos = 0; pos < l_; pos++) {
            int img = getRKElem(q, pos);
            setMatrixElem((size_t)q, (size_t)img,
                          getMatrixElem((size_t)q, (size_t)img) + (l_ - pos));
            setMatrixElem((size_t)img, (size_t)q,
                          getMatrixElem((size_t)img, (size_t)q) + (l_ - pos));
          }
        break;
      case 2:
        for (long q = 0; q < n_; q++)
          for (int pos = 0; pos < l_; pos++) {
            int img = getRKElem(q, pos);
            setMatrixElem((size_t)q, (size_t)img,
                          minZero(getMatrixElem((size_t)q, (size_t)img),
                                  (float)(l_ - pos)));
            setMatrixElem((size_t)img, (size_t)q,
                          minZero(getMatrixElem((size_t)img, (size_t)q),
                                  (float)(l_ - pos)));
          }
        break;
      default:
        break;
    }
    exec_sort_ranked_lists();
  }

  float rbo(long i1, long i2) const {
    float p = 0.7f;
    float inter = 0;
    float result = 0;
    int k = 1;
    while (k <= topK_) {
      inter = 0;
      for (int i = 0; i < k; i++)
        for (int j = 0; j < k; j++)
          if (getRKElem(i1, i) == getRKElem(i2, j)) inter += 1;
      result += std::pow(p, k - 1) * (inter / k);
      k++;
    }
    result = (1 - p) * result;
    return result;
  }

  void compute_rank_correlation() {
    imgCor_.clear();
    imgCor_.resize((size_t)n_);
    for (long q = 0; q < n_; q++) {
      imgCor_[(size_t)q][(int)q] = 1.0;
      for (int pos = 0; pos < topK_; pos++) {
        int img = getRKElem(q, pos);
        imgCor_[(size_t)q][img] = rbo(q, img);
      }
    }
  }

// ---- F5 internals -------------------------------------------------------
  void build_inv() {
    inv_.assign((size_t)n_, {});
    std::vector<int> l1, l2;
    l1.reserve((size_t)topK_);
    l2.reserve((size_t)topK_ * (size_t)topK_);
    for (long q = 0; q < n_; q++) {
      l1.assign(1, (int)q);
      l2.clear();
      for (int i = 1; i < topK_; i++) {
        int imgI = getRKElem(q, i);
        l1.push_back(imgI);
        for (int j = 1; j < topK_; j++) l2.push_back(getRKElem(imgI, j));
      }
      for (int a : l1) inv_[(size_t)a].push_back((uint32_t)q);
      for (int a : l2) inv_[(size_t)a].push_back((uint32_t)q);
    }
    for (long a = 0; a < n_; a++) {
      auto& v = inv_[(size_t)a];
      v.erase(std::unique(v.begin(), v.end()), v.end());
    }
  }

  // Verbatim F5 term sequence for q, keeping only first-index == a. The order
  // of adds per kept cell is the legacy order (b1, then b2 in (i,j) lex, then
  // b3 in j) -- see Source/tests/bfstree_ref_verbatim.hpp.
  inline void replay_q_into_row(int q, int a, float* rowbuf) {
    static thread_local std::vector<int> l1, l2;
    static thread_local std::vector<double> l2d;
    l1.clear();
    l2.clear();
    l2d.clear();
    l1.push_back(q);
    for (int i = 1; i < topK_; i++) {
      int imgI = getRKElem(q, i);
      l1.push_back(imgI);
      float ircor = imgCor_[(size_t)q][imgI];
      for (int j = 1; j < topK_; j++) {
        int imgJ = getRKElem(imgI, j);
        l2.push_back(imgJ);
        l2d.push_back(ircor * imgCor_[(size_t)imgI][imgJ]);
      }
    }
    for (int img1a : l1) {
      if (img1a != a) continue;
      for (int img1b : l1)
        rowbuf[img1b] += imgCor_[(size_t)q][img1a] * imgCor_[(size_t)q][img1b];
    }
    for (size_t i = 0; i < l2.size(); i++) {
      if (l2[i] != a) continue;
      for (size_t j = 0; j < l2.size(); j++)
        rowbuf[l2[j]] += (float)(l2d[i] * l2d[j]);
    }
    for (size_t i = 0; i < l1.size(); i++) {
      if (l1[i] != a) continue;
      for (size_t j = 0; j < l2.size(); j++)
        rowbuf[l2[j]] += imgCor_[(size_t)q][l1[i]] * (float)l2d[j];
    }
  }

  // ---- CSR helpers (same builder shape the engine methods use) -------------
  static M build_csr(idx_t rows, idx_t cols,
                     const std::vector<std::tuple<idx_t, idx_t, float>>& in) {
    M m(rows, cols);
    const int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve((size_t)num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges((size_t)num_threads);
    const size_t chunk =
        ((size_t)rows + (size_t)num_threads - 1) / (size_t)num_threads;
    #pragma omp parallel
    {
      const int tid = omp_get_thread_num();
      auto& allocator = *allocators[(size_t)tid].ptr;
      const idx_t r_start =
          (idx_t)std::min<size_t>((size_t)rows, (size_t)tid * chunk);
      const idx_t r_end =
          (idx_t)std::min<size_t>((size_t)rows, (size_t)(tid + 1) * chunk);
      ranges[(size_t)tid] = {r_start, r_end};
      for (size_t t = 0; t < in.size(); ++t) {
        const idx_t r = std::get<0>(in[t]);
        if (r >= r_start && r < r_end) {
          Triplet* tr = allocator.allocate();
          tr->row = r;
          tr->col = std::get<1>(in[t]);
          tr->value = std::get<2>(in[t]);
          tr->is_set = false;
        }
      }
    }
    M::build_from_partial_results(m, allocators, ranges);
    return m;
  }

  // F8: colsum over stored cells only (zeros are exact no-ops in the double
  // sum) + cell-local divide -- the legacy division pass fused into the read.
  static M normalize_w_prob_sparse(const M& w) {
    const idx_t n = w.rows();
    std::vector<double> colsum((size_t)n, 1.0);
    for (idx_t a = 0; a < n; ++a)
      w.for_each_in_row(a, [&](idx_t c, float v) {
        colsum[(size_t)c] += (double)v;
      });
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve(w.nnz());
    for (idx_t a = 0; a < n; ++a)
      w.for_each_in_row(a, [&](idx_t c, float v) {
        trips.emplace_back(a, c, (float)((double)v / colsum[(size_t)c]));
      });
    return build_csr(n, n, trips);
  }

  // F9 sparse: verbatim sort+pin with matrix[...] replaced by w.get(row,col).
  void rankingSort_sparse(int rk, const M& w) {
    const int l = l_;
    const long LcurRL = ((long)l) * rk;
    std::vector<float> a((size_t)l);
    for (int j = 0; j < l; j++)
      a[(size_t)j] = w.get((idx_t)rk, (idx_t)rk_[LcurRL + j]);
    int i, j, keyR;
    float keyA;
    for (j = 1; j < l; j++) {
      keyA = a[(size_t)j];
      keyR = rk_[LcurRL + j];
      i = j - 1;
      while (i >= 0 && a[(size_t)i] < keyA) {
        a[(size_t)i + 1] = a[(size_t)i];
        rk_[LcurRL + i + 1] = rk_[LcurRL + i];
        i--;
      }
      a[(size_t)i + 1] = keyA;
      rk_[LcurRL + i + 1] = keyR;
    }
    i = 0;
    while ((rk_[LcurRL + i] != rk) && (i < l)) i++;
    if (i > 0) {
      int aux = rk_[LcurRL + 0];
      rk_[LcurRL + 0] = rk_[LcurRL + i];
      rk_[LcurRL + i] = aux;
      float auxA = a[0];
      a[0] = a[(size_t)i];
      a[(size_t)i] = auxA;
    }
  }

  // ---- state --------------------------------------------------------------
  std::vector<int>& rk_;    // rank lists, in place (F9 sorts them)
  long n_;
  int l_;
  int topK_;
  long B_;
  std::vector<float> matrix_;                  // n*n dense (legacy shape)
  std::vector<std::map<int, float>> imgCor_;   // n maps (verbatim F3)
  std::vector<std::vector<uint32_t>> inv_;     // inverted index (F5)
  std::vector<double> colsum_;                 // F6
  M rc_{0, 0};                                 // F7
  M rc8_{0, 0};                                // F8
  long nnz_w_ = 0;
};

#endif  // BFSTREE_CORE_HPP