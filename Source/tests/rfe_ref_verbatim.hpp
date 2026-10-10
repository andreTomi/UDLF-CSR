// rfe_ref_verbatim.hpp
//
// The RFE legacy core, VERBATIM, as the bit-exact reference for the CSR port.
//
// Provenance (do not paraphrase): every algorithm body below is the repo-root
// RFE.cpp (RFE.cpp:34-218, 225-237, 311-451, 450-461, 469-510, 514-561,
// 583-661, 677-715, 717-833)
// with ONLY the framework glue replaced by shims. Each shim is marked "SHIM" and
// listed below. Nothing is reordered, no expression is "simplified", and no
// loop bound is changed -- the reference must stay literal so the pins it
// records are the legacy's, not a re-derivation of "should be equal".
//
// SHIMS (and why each is observationally inert)
//   S1  Udl base class (Config/Exec singletons, InputFileFormat dispatch,
//       prepareInput/prepareOutput, evaluate()) -> absent. The harness is
//       driven directly by the caller with (n, l, k, t, pa). Dead in the
//       in-scope path: verified by call graph, runUdlMethod() never calls
//       prepareInput/prepareOutput.
//   S2  `float* matrix` + `initSparseMatrix(m)` (Udl.cpp:327-341) ->
//       std::vector<float> + assign(n*n, 0.0f). Upstream does
//       `delete[] m; m = new float[nN]; zero every cell` -- nets a zero-fill,
//       which is exactly what value-init gives, without reading
//       uninitialized memory. NET-EQUAL.
//   S3  `rkLists` (Udl-owned, stride l) -> std::vector<int> of size n*l,
//       identical stride and identical in-place mutation by the sorts.
//   S4  VLA `float a[l]` / `float a[l+extra_elems.size()]` ->
//       std::vector<float> of the same length. Same semantics (test_rdpac_ref
//       precedent). Sizes here are fixture-scale (<= a few hundred), so no
//       stack concern is being papered over.
//   S5  std::cout progress prints -> suppressed. They carry no state.
//   S6  `exit(1)` in checkParameters -> throws std::runtime_error. The
//       condition itself is kept verbatim.
//   S7  The fusion and embedding-export halves of the class are NOT here. They
//       are OUT OF SCOPE for this port (deferred) and are not on the path
//       below. The CONNECTED-COMPONENTS half (execRRCCs, RFE.cpp:225-451, plus
//       sortCCEmb at 491-510) WAS added to this reference for the PERFORM_CCS
//       port: reproduced verbatim at the bottom under the same shim
//       discipline, gated by Source/tests/test_rfe_cc_golden.cpp. NOTE: an
//       earlier working-tree attempt (one-shot Python append scripts, since
//       deleted) applied the processCCs predicate as (int)(E*th_cc) and typed
//       th_cc as float; both are WRONG vs the legacy and are re-copied here
//       literally (predicate ((int)E)*th_cc in double; th_cc is double, per
//       repo-root RFE.hpp:140). Hypothesis H11 pins the predicate.
//
// The core reproduced here is exactly the in-scope pipeline:
//
//   rankNorm()
//   for ct in 0..t-1: execEmbeddingRR()   // createEmb + createReverseEmb +
//                                         // combineReverseEmb, then
//                                         // reRankingByEmbSim
//   execCartesianProduct()                // doHyperRREmbedding +
//                                         // doHyperEffecEstimation +
//                                         // reRankingByCPRR
//
// CONTRACT the harness deliberately keeps UNCHANGED (they are inputs, not
// defects to fix here -- the port must reproduce them, and the port is where
// they get a fail-fast):
//   * getPosition() is the O(L) linear scan of the legacy (RFE.cpp:453).
//   * kernelSortRankedLists / kernelSortRankedLists_extra read element i
//     BEFORE testing i < bound (RFE.cpp:755, 816), so a row without its own id
//     at position 0 reads out of bounds. The fixture pins self at 0.
//   * reRankingByCPRR does incElems.resize(n) with NO clear (RFE.cpp:557),
//     while reRankingByCCs does clear() (RFE.cpp:416). Harmless only because
//     CPRR is the first user. Kept verbatim.
//   * initEdgesCCs() mutates the member th_cc (RFE.cpp:353). In scope since the
//     CC extension: the CC harness constructs a FRESH RFERef per
//     fixture x th_cc case, so the mutation cannot leak between cases.
//
// Build posture: the pins this harness records are an -O2, no-fast-math
// contract, exactly like gate_bfstree.sh. -ffp-contract=off is DEFENSIVE here,
// not decisive: rankNorm does exp/log/pow and the Gram accumulates 4-factor
// products, so this is the file that will be most exposed to a fast-math
// build. Measured in Step 4, not assumed.

#ifndef RFE_REF_VERBATIM_HPP
#define RFE_REF_VERBATIM_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// FNV-1a over raw bytes: the instrument for "did these values move", kept
// separate from any md5 of a pass/fail transcript.
inline std::uint64_t rfe_fnv1a(const void* p, std::size_t n,
                                std::uint64_t h = 1469598103934665603ULL) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(b[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

class RFERef {
 public:
  RFERef(int n, int l, int k, int t, double pa)
      : n_(n), l_(l), k_(k), t_(t), pa_(pa) {
    // checkParameters() (RFE.cpp:50-56), verbatim condition.
    if (l_ > n_) {
      throw std::runtime_error("RFE: L can't be greater than N");  // SHIM S6
    }
    // Not in the legacy's checkParameters, but required by its own code and
    // out of its own bounds otherwise: createEmb reads rkLists[l*q + pos1] for
    // pos1 < k (RFE.cpp:589), and rankKScore divides by log(k) (RFE.cpp:645).
    if (k_ > l_) {
      throw std::runtime_error("RFE: need K <= L (createEmb reads rkLists[l*q+pos1])");
    }
    if (k_ < 2) {
      throw std::runtime_error("RFE: need K >= 2 (rankKScore divides by log(K))");
    }
    rkLists_.assign((std::size_t)n_ * (std::size_t)l_, 0);   // SHIM S3
    init_sparse_matrix();                                    // SHIM S2
  }

  // ---------------------------------------------------------------- I/O ----
  void load_rk(const std::vector<int>& rk) { rkLists_ = rk; }
  std::vector<int>& rk() { return rkLists_; }
  // FNV-1a over the rank-list bytes -- the SAME instrument the Step 0 golden uses
  // (same non-canonical offset basis), so the E2E digest is directly comparable
  // to the golden's 0x58ded033b72ded72.
  std::uint64_t ck_rk() const {
    std::uint64_t h = 1469598103934665603ULL;
    const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(rkLists_.data());
    for (std::size_t i = 0; i < rkLists_.size() * sizeof(int); ++i) {
      h ^= b[i];
      h *= 1099511628211ULL;
    }
    return h;
  }
  // F7a scope: reRankingByEmbSim on its own (embRR minus the embedding rebuild,
  // which the caller has already done). exec_embedding_rr() is the full legacy
  // iteration and is what the E2E check uses.
  void embedding_rr_sim_only() { reRankingByEmbSim(); }
  // The two pipeline drivers, exposed for the END-TO-END gate. exec_embedding_rr
  // = doHyperRREmbedding + reRankingByEmbSim (one t-iteration);
  // exec_cartesian_product = doHyperRREmbedding + doHyperEffecEstimation +
  // reRankingByCPRR (the Gram, its tail, and the final rerank).
  void exec_embedding_rr_pub() { exec_embedding_rr(); }
  void exec_cartesian_product_pub() { exec_cartesian_product(); }
  // The CPRR tail ALONE (the set_difference + kernel_sort_ranked_lists_extra
  // loop), for F7b's isolation check. Requires cprr_gram_only() first, which
  // populates incElems, incElemsL and the Gram.
  void cprr_tail_only() {
    for (int q = 0; q < n_; q++) {
      std::set<int> result;
      std::set_difference(incElems_[(std::size_t)q].cbegin(), incElems_[(std::size_t)q].cend(),
                          incElemsL_[(std::size_t)q].cbegin(), incElemsL_[(std::size_t)q].cend(),
                          std::inserter(result, result.cend()));
      kernel_sort_ranked_lists_extra(q, result);
    }
  }
  const std::vector<int>& rk() const { return rkLists_; }

  // ------------------------------------------------------------ accessors --
  const std::vector<float>& matrix() const { return matrix_; }        // SHIM S2
  const std::map<int, std::map<int, float>>& emb() const { return emb_; }
  const std::map<int, std::map<int, float>>& remb() const { return remb_; }
  // H4 needs the pre-combine snapshot: combineReverseEmb MUTATES emb_ in place
  // (RFE.cpp:639), so once it has run the "before" state is gone. The caller
  // must snapshot before calling combine_reverse_emb().
  std::map<int, std::map<int, float>> emb_copy() const { return emb_; }
  std::map<int, std::map<int, float>> remb_copy() const { return remb_; }
  const std::vector<float>& ee() const { return ee_; }
  const std::vector<int>& sorted_ee() const { return sorted_ee_; }
  const std::vector<std::vector<int>>& sorted_emb() const { return sorted_emb_; }
  const std::vector<std::set<int>>& inc_elems() const { return incElems_; }
  const std::vector<std::set<int>>& inc_elems_l() const { return incElemsL_; }
  // The divisor, i.e. the max UNNORMALISED ee. Not recoverable from ee() after the
  // fact (everything gets scaled by it), so it is recorded explicitly for the gate.
  float gr_value_probe() const { return gr_value_; }
  // Direct read of one Gram cell. The gate compares off-diagonal cells only,
  // because the diagonal is OVERWRITTEN with pow(2,30) after accumulation
  // (RFE.cpp:605) and that pin belongs to F7's input state, not to F6's sums.
  float gram_at(int i, int j) const { return MX(i, j); }

  // Structural nnz of the dense `matrix` as the legacy holds it: the number of
  // cells that are not the zero-fill. Reported per phase. This is the count the
  // CSR port has to reproduce; it is NOT the same as "the nonzeros the port
  // keeps", because the port prunes the Gram (see the plan).
  std::size_t matrix_nnz() const {
    std::size_t c = 0;
    for (float v : matrix_)
      if (v != 0.0f) ++c;
    return c;
  }

  void init_sparse_matrix() { matrix_.assign((std::size_t)n_ * (std::size_t)n_, 0.0f); }  // SHIM S2

  // ------------------------------------------------------- census (SHIM S8) --
  // Measurement helpers. These are NOT part of the legacy; they only read the
  // state the legacy produced and summarise it, so the driver can assert the
  // structural claims of the plan (chief among them: `emb` carries keys whose
  // value is EXACTLY zero, and those keys are load-bearing).
  std::size_t emb_total_keys() const {
    std::size_t c = 0;
    for (const auto& kv : emb_) c += kv.second.size();
    return c;
  }
  std::size_t emb_exact_zero_keys() const {
    std::size_t c = 0;
    for (const auto& kv : emb_)
      for (const auto& e : kv.second)
        if (e.second == 0.0f) ++c;
    return c;
  }
  std::size_t emb_negative_keys() const {
    std::size_t c = 0;
    for (const auto& kv : emb_)
      for (const auto& e : kv.second)
        if (e.second < 0.0f) ++c;
    return c;
  }
  // Number of keys of emb[q] whose reverse partner emb[key][q] is absent (or
  // zero): exactly the entries combineReverseEmb multiplies by zero.
  std::size_t emb_keys_without_reverse() const {
    std::size_t c = 0;
    for (const auto& kv : emb_)
      for (const auto& e : kv.second) {
        const auto rit = emb_.find(e.first);
        if (rit == emb_.end() || rit->second.find(kv.first) == rit->second.end())
          ++c;
      }
    return c;
  }
  // Support saturation: |supp(emb)| / n^2. At the pinned fixture this is ~0.32;
  // it approaches 1.0 as k approaches L, at which point emb is effectively
  // DENSE and the exact-zero / no-reverse-partner populations are absorbed.
  //
  // MEASURED REGIME DEPENDENCE (step0_probe_k.sh, N=200 L=40). This is the
  // briefing's rule 2 in miniature -- synthetic small-N saturates:
  //   k= 3 -> keys  1200 (3% of n^2), zeros 916 (76% of keys)
  //   k=10 -> keys 12812 (32%),          zeros 7910 (62%)
  //   k=25 -> keys 37702 (94%),          zeros 3212 (8.5%)
  //   k=40 -> keys 39995 (100%),         zeros   21 (0.05%)
  // The keys are still INSERTED at every k (incEmb's else-branch is
  // unconditional); the 0.0f simply stops being the final value once other pos1
  // terms have written the same column, because `acc += 0.0f` is a no-op. So
  // "emb carries exact zeros" is TRUE IN THE SPARSE REGIME and effectively
  // unobservable in the saturated one -- NOT a property of the method alone.
  // Whether it holds at production k (ALOI N=72k, L=400, k=20) is a Step 3
  // real-data question, not answered by this fixture.
  double emb_saturation() const {
    const double n2 = (double)n_ * (double)n_;
    return n2 > 0.0 ? (double)emb_total_keys() / n2 : 0.0;
  }
  // |incElems| total: the candidate set the Gram DISCOVERED beyond the rank
  // lists. This is the quantity that decides whether the Gram needs pruning.
  std::size_t inc_elems_total() const {
    std::size_t c = 0;
    for (const auto& s : incElems_) c += s.size();
    return c;
  }
  std::size_t inc_elems_max_row() const {
    std::size_t m = 0;
    for (const auto& s : incElems_) m = std::max(m, s.size());
    return m;
  }

public:
  // ------------------------------------------------------------- driver ----
  // runUdlMethod() (RFE.cpp:102-124) restricted to the in-scope core (SHIM S7:
  // performCCs / rerankByEmb / exportEmbeddings are false here, so the legacy's
  // execRRCCs() and execRRByEmbCCs() calls never fire and are not reproduced).
  // The per-phase calls are left visible so the driver can instrument between
  // them; nothing is hoisted or fused.
  void run() {
    rankNorm();
    for (int ct = 0; ct < t_; ct++) exec_embedding_rr();
    exec_cartesian_product();
  }
  // Named wrappers so the driver can call the legacy's phases individually with
  // their legacy names (no body duplication).
  void rank_norm() { rankNorm(); }
  void embedding_rr() { exec_embedding_rr(); }
  void cartesian_product() { exec_cartesian_product(); }

  // ---- Step 1 (hypothesis gate) seam -------------------------------------
  // Step 0 only needed run()/rank_norm()/embedding_rr()/cartesian_product().
  // The Step 1 gate has to observe emb BEFORE and AFTER combineReverseEmb, and
  // must be able to run the Gram WITHOUT the trailing re-rank, so that the Gram
  // can be compared against a CSR kernel at the same phase boundary.
  //
  // These are WRAPPERS ONLY -- each forwards to the legacy body, which is
  // untouched. No arithmetic is moved, duplicated or reordered here; that is the
  // whole point of keeping them one-line forwards.
  void create_emb() { createEmb(); }
  void create_reverse_emb() { createReverseEmb(); }
  void combine_reverse_emb() { combineReverseEmb(); }
  void hyper_rr_embedding() { do_hyper_rr_embedding(); }
  void effec_estimation() { do_hyper_effec_estimation(); }
  void sort_emb() { sortEmb(); }
  void effect_estimation_emb() { effectEstimationEmb(); }
  void cprr() { reRankingByCPRR(); }
  // F6 scope: the Gram half only (no set_difference / final sort). The F7 rerank
  // and the final kernel_sort_ranked_lists_extra loop are separate phases.
  void cprr_gram_only() { cprr_gram_prefix(); }

  // getPosition is private in the legacy; exposed for H9 (the O(L) scan vs a
  // position index). Same body, verbatim.
  unsigned int get_position(int q_img, int img) { return getPosition(q_img, img); }

  // simEmb is private in the legacy; exposed for H5 (merge-join equivalence).
  // Same body, verbatim -- the test compares the merge against THIS.
  float sim_emb_pub(const std::map<int, float>& di, const std::map<int, float>& dj) {
    return simEmb(di, dj);
  }

  int n() const { return n_; }
  int l() const { return l_; }
  int k() const { return k_; }
  int t() const { return t_; }
  double pa() const { return pa_; }

  // ---- CC seam (the legacy's private execRRCCs et al., exposed for the gate) --
  // One-line wrappers only; every body below is the legacy's, verbatim.
  void exec_rr_ccs() { execRRCCs(); }
  void create_edges_ccs() { createEdgesCCS(); }
  void process_ccs() { processCCs(); }
  void init_edges_ccs() { initEdgesCCs(); }
  void compute_ccs_emb() { computeCCsEmb(); }
  void compute_embs_by_ccs() { computeEmbsByCCs(); }
  void reranking_by_ccs() { reRankingByCCs(); }
  // H14 seam: unionCCs/sameCC exposed so the gate can drive directed graphs
  // (chain, star, reversed edge) against an independent DSU.
  void union_ccs(int o1, int o2) { unionCCs(o1, o2); }
  bool same_cc(int o1, int o2) { return sameCC(o1, o2); }
  // th_cc comes from PARAM_RFE_TH_CC in the legacy (repo-root RFE.hpp:140,
  // double). The harness sets it before exec_rr_ccs(); 0.0 selects the
  // legacy's automatic average path (initEdgesCCs, RFE.cpp:348-355).
  void set_th_cc(double v) { th_cc_ = v; }
  double th_cc() const { return th_cc_; }

  // Read-only views for the CC gate's stage digests.
  const std::vector<std::pair<float, std::pair<int, int>>>& edges_ccs() const {
    return edgesCCs_;
  }
  const std::map<int, std::set<int>>& rcc() const { return rcc_; }
  const std::map<int, int>& icc() const { return icc_; }
  const std::map<int, std::map<int, float>>& ccemb() const { return ccemb_; }
  const std::vector<std::vector<int>>& sorted_ccemb() const { return sorted_ccemb_; }
  const std::map<int, std::map<int, float>>& embycc() const { return embycc_; }

 private:
  // ---- RFE.cpp:180-193 ----------------------------------------------------
  void rankNorm() {
    for (int q = 0; q < n_; q++) {
      for (int i = 0; i < l_; i++) {
        int img_rl_i = rkLists_[(std::size_t)l_ * q + i];
        int posqi = i + 1;
        int posiq = getPosition(img_rl_i, q);   // arg order per legacy (see below)
        float newSim =
            (pow(rankScoreSigmoid(posqi, k_ / 2), 2) * rankScoreSigmoid(posiq, k_ / 2));
        MX(q, img_rl_i) = newSim;
      }
    }
    exec_sort_ranked_lists();
  }

  // ---- RFE.cpp:195-201 ----------------------------------------------------
  // Two narrowings are load-bearing and reproduced literally: `sigmoidv` is
  // double-rounded into a float, THEN `1 - sigmoidv` is float arithmetic; and
  // `1/(1+exp(...))` is a true division (`1` promoted), not an int division.
  float rankScoreSigmoid(int x, int pk) {
    if (x == 1) return 1;
    float sigmoidv = 1 / (1 + exp((-pa_) * (x - pk)));
    return 1 - sigmoidv;
  }

  // ---- RFE.cpp:203-212 ----------------------------------------------------
  void exec_embedding_rr() {
    do_hyper_rr_embedding();
    reRankingByEmbSim();
  }
  void do_hyper_rr_embedding() {
    createEmb();
    createReverseEmb();
    combineReverseEmb();
  }
  void exec_cartesian_product() {
    do_hyper_rr_embedding();
    do_hyper_effec_estimation();
    reRankingByCPRR();
  }

  // ---- RFE.cpp:220-223 ----------------------------------------------------
  void do_hyper_effec_estimation() {
    sortEmb();
    effectEstimationEmb();
  }

// ---- RFE.cpp:453-460 ----------------------------------------------------
  // VERBATIM O(L) scan. The `l + 2` return is load-bearing: it feeds
  // rankScoreSigmoid for a neighbour that does not list us back. Called from
  // rankNorm as getPosition(img_rl_i, q) -- the LEGACY'S ARGUMENT ORDER, i.e.
  // "where does q sit in img_rl_i's list", the reciprocal rank, NOT the
  // position of img_rl_i in q's own list. reRankingByCCs (deferred) calls it
  // the other way round; do not normalise these into one helper.
  unsigned int getPosition(unsigned int qImg, unsigned int img) {
    for (int i = 0; i < l_; i++) {
      if (rkLists_[(std::size_t)l_ * qImg + i] == (int)img) {
        return (unsigned int)(i + 1);
      }
    }
    return (unsigned int)(l_ + 2);
  }

  // ---- RFE.cpp:583-600 ----------------------------------------------------
  // The accumulation order is by pos1 (RANK POSITION), with term 1 of pos1
  // emitted BEFORE the k terms 2 of that same pos1. This is NOT "all of S, then
  // all of S*S", and a column-ordered SpGEMM replays a different permutation.
  // rankKScore(k) == 0.0f exactly (log(k)/log(k) == 1.0), so pos1 = k-1 writes
  // EXACT zeros through incEmb, which INSERTS the key. Those keys survive into
  // sorted_emb and into incElems. Do not "clean" this.
  void createEmb() {
    emb_.clear();
    for (int q = 0; q < n_; q++) {
      std::map<int, float> qemb;
      for (int pos1 = 0; pos1 < k_; pos1++) {
        int img_n = rkLists_[(std::size_t)l_ * q + pos1];
        float wn = rankKScore(pos1 + 1);
        incEmb(qemb, img_n, wn);
        for (int pos2 = 0; pos2 < k_; pos2++) {
          float wnn = rankKScore(pos2 + 1);
          int img_nn = rkLists_[(std::size_t)l_ * img_n + pos2];
          incEmb(qemb, img_nn, wn * wnn);
        }
      }
      emb_[q] = qemb;
    }
  }

  // ---- RFE.cpp:618-632 ----------------------------------------------------
  // emb^T, exactly: each (q,key) pair of emb is visited once, so the `+=` is a
  // store. Order irrelevant (no accumulation), hence transpose() is exact.
  void createReverseEmb() {
    remb_.clear();
    for (int q = 0; q < n_; q++) {
      std::map<int, float> qremb;
      remb_[q] = qremb;
    }
    for (int q = 0; q < n_; q++) {
      std::map<int, float> qemb = emb_[q];
      for (auto it = qemb.cbegin(); it != qemb.cend(); it++) {
        auto key = it->first;
        incEmb(remb_[key], q, qemb[key]);
      }
    }
  }

  // ---- RFE.cpp:634-642 ----------------------------------------------------
  // NOT CSRMatrix::combine and NOT merge(): `remb_[q][key]` is operator[] on a
  // std::map, so a missing reverse partner INSERTS 0.0f and the product is 0
  // -- while the KEY STAYS in emb[q]. The support of emb is unchanged; only the
  // values move. Any port that returns the intersection of the supports is
  // wrong here, in support, not merely in the last bits.
  void combineReverseEmb() {
    for (int q = 0; q < n_; q++) {
      std::map<int, float> qemb = emb_[q];
      for (auto it = qemb.cbegin(); it != qemb.cend(); it++) {
        auto key = it->first;
        emb_[q][key] = (emb_[q][key]) * (remb_[q][key]);
      }
    }
  }

  // ---- RFE.cpp:644-646 ----------------------------------------------------
  float rankKScore(int pos) { return (1 - (log(pos) / log(k_))); }

  // ---- RFE.cpp:677-686 ----------------------------------------------------
  // Iterates di (emb[q]) in ASCENDING KEY ORDER and looks dj up: for a CSR
  // row pair both sorted by column, a two-pointer merge replays this order
  // bit-for-bit. This is the bit-exact kernel shape for reRankingByEmbSim.
  float simEmb(const std::map<int, float>& di, const std::map<int, float>& dj) {
    float sim = 0;
    for (auto& pair : di) {
      auto dj_iter = dj.find(pair.first);
      if (dj_iter != dj.end()) {
        sim += pair.second * dj_iter->second;
      }
    }
    return sim;
  }

  // ---- RFE.cpp:688-694 ----------------------------------------------------
  // The `else d[img] = value` branch is why exact zeros become KEYS.
  void incEmb(std::map<int, float>& d, int img, float value) {
    if (d.find(img) != d.end()) {
      d[img] += value;
    } else {
      d[img] = value;
    }
  }

// ---- RFE.cpp:648-661 ----------------------------------------------------
  // Writes exactly l cells per row (assignment, not +=). `pos` starts at 1 and
  // increments AFTER the store, so the divisor for row-slot i is i+1.
  void reRankingByEmbSim() {
    init_sparse_matrix();
    for (int q = 0; q < n_; q++) {
      int pos = 1;
      for (int i = 0; i < l_; i++) {
        int img_i = rkLists_[(std::size_t)l_ * q + i];
        float newSim = (simEmb(emb_[q], emb_[img_i])) / pos;
        MX(q, img_i) = newSim;
        pos += 1;
      }
    }
    exec_sort_ranked_lists();
  }

// ---- RFE.cpp:717-767 ----------------------------------------------------
  void exec_sort_ranked_lists() {
    for (int i = 0; i < n_; i++) kernel_sort_ranked_lists(i);
  }

  // Insertion sort (STABLE: the loop shifts only on strict `<`), then self is
  // moved to position 0 unconditionally. The self pin reads rkLists[LcurRL+i]
  // before testing i < l_ (RFE.cpp:755): the fixture guarantees self at 0.
  void kernel_sort_ranked_lists(int rk) {
    long int LcurRL = (long int)l_ * rk;
    std::vector<float> a((std::size_t)l_);                    // SHIM S4

    for (int j = 0; j < l_; j++) {
      a[(std::size_t)j] = MX(rk, rkLists_[(std::size_t)LcurRL + j]);
    }

    int i, j, keyR;
    float keyA;
    for (j = 1; j < l_; j++) {
      keyA = a[(std::size_t)j];
      keyR = rkLists_[(std::size_t)LcurRL + j];
      i = j - 1;
      while (i >= 0 && a[(std::size_t)i] < keyA) {
        a[(std::size_t)i + 1] = a[(std::size_t)i];
        rkLists_[(std::size_t)LcurRL + i + 1] = rkLists_[(std::size_t)LcurRL + i];
        i--;
      }
      a[(std::size_t)i + 1] = keyA;
      rkLists_[(std::size_t)LcurRL + i + 1] = keyR;
    }

    i = 0;
    while ((rkLists_[(std::size_t)LcurRL + i] != rk) && (i < l_)) {
      i++;
    }
    if (i > 0) {
      int aux = rkLists_[(std::size_t)LcurRL + 0];
      rkLists_[(std::size_t)LcurRL + 0] = rkLists_[(std::size_t)LcurRL + i];
      rkLists_[(std::size_t)LcurRL + i] = aux;

      float auxA = a[0];
      a[0] = a[(std::size_t)i];
      a[(std::size_t)i] = auxA;
    }
  }

// ---- RFE.cpp:465-467 ----------------------------------------------------
  // Free function in the legacy; kept as a functor with the same comparison
  // (descending by value, ties left to std::sort's instability).
  struct CmpDesc {
    bool operator()(const std::pair<int, float>& a, const std::pair<int, float>& b) const {
      return a.second > b.second;
    }
  };

  // ---- RFE.cpp:469-489 ----------------------------------------------------
  // std::sort is UNSTABLE, so the order of equal-valued keys is libstdc++
  // specific. Kept verbatim. (The plan shows ee[q] is INVARIANT to this
  // tie-break -- ties are adjacent and a+b == b+a in IEEE-754 -- but
  // sorted_ee below is a different story and DOES matter bit-for-bit.)
  void sortEmb() {
    sorted_emb_.clear();
    sorted_emb_.resize((std::size_t)n_);
    for (int q = 0; q < n_; q++) {
      std::vector<std::pair<int, float>> A;
      for (auto& it : emb_[q]) {
        A.push_back(it);
      }
      std::sort(A.begin(), A.end(), CmpDesc());
      for (auto& it : A) {
        sorted_emb_[(std::size_t)q].push_back(it.first);
      }
    }
  }

  // ---- RFE.cpp:513-525 ----------------------------------------------------
  // std::sort over an index permutation, descending by value, UNSTABLE. This
  // ORDER is the Gram's accumulation order, so ties here are bit-relevant.
  std::vector<int> tag_sort(const std::vector<float>& v) {
    std::vector<int> resul(v.size());
    std::iota(std::begin(resul), std::end(resul), 0);
    std::sort(std::begin(resul), std::end(resul),
              [&v](const auto& lhs, const auto& rhs) { return v[(std::size_t)lhs] > v[(std::size_t)rhs]; });
    return resul;
  }

  // ---- RFE.cpp:527-550 ----------------------------------------------------
  // Sums the first k keys of sorted_emb[q], then normalises by the MAXIMUM ee.
  // If grValue were 0 this divides by zero; kept verbatim.
  void effectEstimationEmb() {
    ee_.clear();
    ee_.resize((std::size_t)n_);
    for (int q = 0; q < n_; q++) {
      auto qemb = emb_[q];
      float eeValue = 0;
      int pos = 0;
      for (int& key : sorted_emb_[(std::size_t)q]) {
        pos++;
        if (pos > k_) {
          break;
        }
        eeValue += emb_[q][key];
      }
      ee_[(std::size_t)q] = eeValue;
    }
    sorted_ee_ = tag_sort(ee_);
    int key = sorted_ee_[0];
    float grValue = ee_[(std::size_t)key];
    gr_value_ = grValue;
    for (int q = 0; q < n_; q++) {
      ee_[(std::size_t)q] = ee_[(std::size_t)q] / grValue;
    }
  }

// ---- RFE.cpp:696-715 ----------------------------------------------------
  // NOTE the two guards: a key of sorted_emb is skipped if it is not in emb.
  // And note incElems[i].insert(j) fires for EVERY visited pair, regardless of
  // value -- so a pair whose product is exactly 0.0f still DISCOVERS a
  // candidate. This is what makes the exact-zero keys of emb load-bearing, and
  // it is why a port that prunes |v| <= 1e-9 (as multiply_and_prune does at
  // SparseMatrix.hpp:559) changes the OUTPUT, not just the last bits.
  void incSimByECP(int qi, int qj, float wi, float wj) {
    const auto& iemb = emb_[qi];
    const auto& jemb = emb_[qj];

    for (const int i : sorted_emb_[(std::size_t)qi]) {
      auto it_i = iemb.find(i);
      if (it_i == iemb.end()) continue;
      float vi = it_i->second;

      for (const int j : sorted_emb_[(std::size_t)qj]) {
        auto it_j = jemb.find(j);
        if (it_j == jemb.end()) continue;
        float vj = it_j->second;

        incElems_[(std::size_t)i].insert(j);
        MX(i, j) += (vi * vj) * wi * wj;
      }
    }
  }

  // ---- RFE.cpp:552-581 ----------------------------------------------------
  // The Gram: matrix[i][j] = sum_q (emb[q][i]*emb[q][j]) * ee[q] * 1, in
  // sorted_ee order (hence a libstdc++-tie-dependent ORDER). wi = ee[q],
  // wj = 1 CONSTANT -- the call site passes literal 1 (RFE.cpp:561), so the
  // float sequence is (vi*vj)*wi*1. incElems.resize(n) with NO clear is
  // verbatim (safe only because CPRR is the first user). The diagonal pin
  // matrix[n*q+q] = pow(2,30) OVERWRITES after all accumulation.
  // The Gram half of reRankingByCPRR, WITHOUT the final kernel_sort_ranked_lists_extra
  // call. F6 owns the accumulation and the candidate sets; the re-ranking and the
  // set_difference output are F7. Splitting them here keeps each phase's parity
  // claim scoped to the phase that actually produces the state under test.
  //
  // The FULL legacy phase (RFE.cpp:552-581) is reRankingByCPRR, which calls this
  // and then appends the set_difference + kernel_sort_ranked_lists_extra loop.
  // Both live here so the reference keeps one verbatim source for the shared
  // prefix rather than two copies that could drift apart.
  void cprr_gram_prefix() {
    init_sparse_matrix();

    incElems_.resize((std::size_t)n_);
    incElemsL_.resize((std::size_t)n_);
    for (int q : sorted_ee_) {
      float value = ee_[(std::size_t)q];
      incSimByECP(q, q, value, 1);
    }

    for (int q = 0; q < n_; q++) {
      int pos = 1;
      for (int i = 0; i < l_; i++) {
        int img_i = rkLists_[(std::size_t)l_ * q + i];
        pos++;
        incElemsL_[(std::size_t)q].insert(img_i);
      }
      MX(q, q) = pow(2, 30);
    }
    // The set_difference + kernel_sort_ranked_lists_extra loop below is F7.
  }

  // The FULL legacy phase, verbatim (RFE.cpp:552-581): the shared Gram prefix, then
  // the set_difference and the extra-element sort. F6 gates the prefix; F7 gates
  // the tail. The prefix is NOT re-inlined here -- calling cprr_gram_prefix() keeps
  // one verbatim source for it, so the two phases cannot drift apart.
  void reRankingByCPRR() {
    cprr_gram_prefix();

    for (int q = 0; q < n_; q++) {
      std::set<int> result;
      std::set_difference(incElems_[(std::size_t)q].cbegin(), incElems_[(std::size_t)q].cend(),
                          incElemsL_[(std::size_t)q].cbegin(), incElemsL_[(std::size_t)q].cend(),
                          std::inserter(result, result.cend()));
      kernel_sort_ranked_lists_extra(q, result);
    }
  }

// ---- RFE.cpp:769-833 ----------------------------------------------------
  // Concatenates the current list with the discovered extras (ascending id, by
  // std::set), insertion-sorts ASCENDING by similarity, then copies back only
  // the first l entries. Extras whose matrix cell was never touched read the
  // zero-fill. The self pin reads rkTmp[i] before testing i < new_size
  // (RFE.cpp:816) -- self is at 0 by fixture.
  void kernel_sort_ranked_lists_extra(int rk, std::set<int> extra_elems) {
    std::size_t cap = (std::size_t)l_ + extra_elems.size();
    std::vector<float> a(cap);                             // SHIM S4

    std::vector<int> rkTmp;
    rkTmp.resize(cap);

    int pos = 0;
    for (int i = 0; i < l_; i++) {
      rkTmp[(std::size_t)pos] = rkLists_[(std::size_t)l_ * rk + i];
      pos++;
    }
    for (int img : extra_elems) {
      rkTmp[(std::size_t)pos] = img;
      pos++;
    }

    int new_size = (int)rkTmp.size();

    for (int j = 0; j < new_size; j++) {
      a[(std::size_t)j] = MX(rk, rkTmp[(std::size_t)j]);
    }

    int i, j, keyR;
    float keyA;
    for (j = 1; j < new_size; j++) {
      keyA = a[(std::size_t)j];
      keyR = rkTmp[(std::size_t)j];
      i = j - 1;
      while (i >= 0 && a[(std::size_t)i] < keyA) {
        a[(std::size_t)i + 1] = a[(std::size_t)i];
        rkTmp[(std::size_t)i + 1] = rkTmp[(std::size_t)i];
        i--;
      }
      a[(std::size_t)i + 1] = keyA;
      rkTmp[(std::size_t)i + 1] = keyR;
    }

    i = 0;
    while ((rkTmp[(std::size_t)i] != rk) && (i < new_size)) {
      i++;
    }
    if (i > 0) {
      int aux = rkTmp[0];
      rkTmp[0] = rkTmp[(std::size_t)i];
      rkTmp[(std::size_t)i] = aux;

      float auxA = a[0];
      a[0] = a[(std::size_t)i];
      a[(std::size_t)i] = auxA;
    }

    for (int i2 = 0; i2 < l_; i2++) {
      rkLists_[(std::size_t)l_ * rk + i2] = rkTmp[(std::size_t)i2];
    }
  }

  int n_, l_, k_, t_;
  double pa_;
  std::vector<int> rkLists_;                                    // SHIM S3
  std::vector<float> matrix_;                                   // SHIM S2
  std::map<int, std::map<int, float>> emb_;
  std::map<int, std::map<int, float>> remb_;
  std::vector<std::vector<int>> sorted_emb_;
  std::vector<float> ee_;
  std::vector<int> sorted_ee_;
  float gr_value_ = 0.0f;
  std::vector<std::set<int>> incElems_;
  std::vector<std::set<int>> incElemsL_;

  float& MX(int i, int j) { return matrix_[(std::size_t)n_ * (std::size_t)i + (std::size_t)j]; }  // SHIM S2
  float MX(int i, int j) const { return matrix_[(std::size_t)n_ * (std::size_t)i + (std::size_t)j]; }  // SHIM S2

  // =========================================================================
  // CC (Connected Components) -- execRRCCs (RFE.cpp:225-230) and every phase
  // it reaches, VERBATIM from RFE.cpp:225-451 and 491-510. Shim notes: member
  // names carry the trailing underscore like the rest of this class; th_cc_ is
  // the legacy's double member (repo-root RFE.hpp:140) defaulting to 0.0 and
  // settable through set_th_cc (the legacy reads PARAM_RFE_TH_CC from config);
  // progress prints suppressed (S5). Everything expensive or dubious is KEPT,
  // because the pins must be the legacy's, defects included: the
  // set_union-inserter writing into its own source range (RFE.cpp:369), the
  // `for (auto t : edgesCCs)` tuple copy, the two full copies of each ccemb map
  // per reRankingByCCs iteration, the double arithmetic of newSim rounded to
  // float on assignment, and the O(L) getPosition.
  // =========================================================================

  // ---- RFE.cpp:225-230 -- execRRCCs -----------------------------------------
  void execRRCCs() {
    do_hyper_rr_embedding();
    do_hyper_effec_estimation();
    doCCsEmbedding();
    reRankingByCCs();
  }

  // ---- RFE.cpp:232-237 -- doCCsEmbedding ------------------------------------
  void doCCsEmbedding() {
    createEdgesCCS();
    processCCs();
    computeCCsEmb();
    computeEmbsByCCs();
  }

  // ---- RFE.cpp:311-328 -- createEdgesCCS ------------------------------------
  // NOTE: no clear() before push_back -- verbatim; the legacy calls this once
  // per path. confValue associates left-to-right: (simEmb * ee[q]) * ee[nn],
  // all float. The sort is lexicographic DESCENDING over (conf, q, nn).
  void createEdgesCCS() {
    for (int q = 0; q < n_; q++) {
      for (int i = 0; i < k_; i++) {
        int img_nn = rkLists_[(std::size_t)l_ * q + (std::size_t)i];
        if (q == img_nn) {  // skip query
          continue;
        }
        float confValue = (simEmb(emb_[q], emb_[img_nn]) * ee_[(std::size_t)q] *
                           ee_[(std::size_t)img_nn]);
        std::pair<int, int> img_pair = {q, img_nn};
        std::pair<float, std::pair<int, int>> tup = {confValue, img_pair};
        edgesCCs_.push_back(tup);
      }
    }
    // Using simple sort() function to sort
    std::sort(edgesCCs_.rbegin(), edgesCCs_.rend());
  }

  // ---- RFE.cpp:330-344 -- processCCs ----------------------------------------
  // The bound is ((int)E) * th_cc in DOUBLE (the cast binds to size(), not to
  // the product) -- do not "simplify" it into (int)(E*th_cc): H11 pins that the
  // two can select different prefixes.
  void processCCs() {
    initEdgesCCs();
    int i = 0;
    for (auto t : edgesCCs_) {
      if (i >= ((int)edgesCCs_.size() * th_cc_)) {
        break;
      }
      std::pair<int, int> img_pair = t.second;
      if (!(sameCC(img_pair.first, img_pair.second))) {
        unionCCs(img_pair.first, img_pair.second);
      }
      i++;
    }
  }

  // ---- RFE.cpp:346-363 -- initEdgesCCs --------------------------------------
  void initEdgesCCs() {
    // CC Threshold
    if (th_cc_ == 0) {
      float avg = 0;
      for (int q : sorted_ee_) {
        avg += ee_[(std::size_t)q];
      }
      avg = avg / n_;
      th_cc_ = avg / 2;
    }
    // CCs Structures
    rcc_.clear();
    icc_.clear();
    for (int q = 0; q < n_; q++) {
      rcc_[(std::size_t)q] = std::set<int>{q};
      icc_[(std::size_t)q] = q;
    }
  }

  // ---- RFE.cpp:365-375 -- unionCCs ------------------------------------------
  // r1 (the label of the o1/q side) survives and becomes the ccemb key; the
  // iteration order of reRankingByCCs over ccemb is therefore load-bearing.
  // The inserter writes into rcc_[r1] WHILE rcc_[r1] is itself one of the
  // source ranges (RFE.cpp:369) -- formally UB, practically the set-union of
  // two node-based sets (no iterator invalidation). KEPT; H14 pins the
  // observed behaviour against an independent DSU.
  void unionCCs(int o1, int o2) {
    int r1 = icc_[(std::size_t)o1];
    int r2 = icc_[(std::size_t)o2];

    std::set_union(rcc_[(std::size_t)r2].cbegin(), rcc_[(std::size_t)r2].cend(),
                   rcc_[(std::size_t)r1].cbegin(), rcc_[(std::size_t)r1].cend(),
                   std::inserter(rcc_[(std::size_t)r1], rcc_[(std::size_t)r1].cend()));
    for (auto o : rcc_[(std::size_t)r2]) {
      icc_[(std::size_t)o] = r1;
    }

    rcc_.erase((std::size_t)r2);
  }

  // ---- RFE.cpp:377-379 -- sameCC --------------------------------------------
  bool sameCC(int o1, int o2) {
    return (icc_[(std::size_t)o1] == icc_[(std::size_t)o2]);
  }

  // ---- RFE.cpp:381-398 -- computeCCsEmb -------------------------------------
  // Only CCs with >1 member enter. The sum is float, over members in ASCENDING
  // id order (std::set), and within each member over ascending key order
  // (std::map). Keys whose final value is exactly 0.0f are structural: the map
  // keeps every key any member carried. sorted_ccemb's keys are EMBEDDING
  // OBJECTS, not CC members.
  void computeCCsEmb() {
    ccemb_.clear();
    for (auto it1 : rcc_) {
      int r = it1.first;
      std::set<int> d = it1.second;
      if (d.size() > 1) {
        std::map<int, float> curemb;
        for (auto o : rcc_[(std::size_t)r]) {
          for (auto it2 : emb_[o]) {
            int key = it2.first;
            curemb[key] = curemb[key] + emb_[o][key];  // original
          }
        }
        ccemb_[(std::size_t)r] = curemb;
      }
    }
    sortCCEmb();
  }

  // ---- RFE.cpp:400-413 -- computeEmbsByCCs ----------------------------------
  // DENSE over every CC label: curCCEmb[r] = simEmb(emb[q], ccemb[r]) for all
  // q and all r, including exact zeros. `qemb` and `ccd` are copies -- verbatim
  // (auto by value at RFE.cpp:403, 407).
  void computeEmbsByCCs() {
    embycc_.clear();
    for (int q = 0; q < n_; q++) {
      auto qemb = emb_[q];
      std::map<int, float> curCCEmb;
      for (auto it : ccemb_) {
        int r = it.first;
        auto ccd = it.second;
        float vccemb = simEmb(qemb, ccd);  // original
        curCCEmb[r] = vccemb;
      }
      embycc_[q] = curCCEmb;
    }
  }

  // ---- RFE.cpp:491-510 -- sortCCEmb -----------------------------------------
  // Same shape as sortEmb: std::sort UNSTABLE with the strict-`>` comparator
  // over a vector built in ascending key order (libstdc++-specific tie order).
  // sorted_ccemb_.resize(n) does NOT clear a previous run -- verbatim (safe:
  // the legacy calls this once per path; the harness uses a fresh RFERef per
  // case).
  void sortCCEmb() {
    sorted_ccemb_.resize((std::size_t)n_);
    for (auto iterator : ccemb_) {
      int q = iterator.first;
      std::vector<std::pair<int, float>> A;
      for (auto& it : ccemb_[(std::size_t)q]) A.push_back(it);
      std::sort(A.begin(), A.end(), CmpDesc());
      for (auto& it : A) {
        sorted_ccemb_[(std::size_t)q].push_back(it.first);
      }
    }
  }

  // ---- RFE.cpp:415-451 -- reRankingByCCs ------------------------------------
  // newSim is DOUBLE arithmetic -- sqrt of an int product (-> ::sqrt(double)),
  // division by the unsigned getPosition -- rounded to float only on the
  // assignment. Writes are plain assignments: when several CCs write the same
  // (k1,k2) cell, the LAST CC in ascending-label order wins. getPosition reads
  // the rank lists POST-f7b and returns l+2 for an absent item. Unlike CPRR's
  // Gram this phase has NO diagonal pin; untouched cells read the zero-fill
  // and the tail sorts them as ordinary zeros.
  void reRankingByCCs() {
    incElems_.clear();
    incElemsL_.clear();
    incElems_.resize((std::size_t)n_);
    incElemsL_.resize((std::size_t)n_);

    init_sparse_matrix();
    for (auto it : ccemb_) {
      int r = it.first;
      std::map<int, float> d = it.second;
      for (int i = 0; i < std::min(k_, ((int)d.size())); i++) {
        int k1 = sorted_ccemb_[(std::size_t)r][(std::size_t)i];
        for (int j = 0; j < std::min(k_, ((int)d.size())); j++) {
          int k2 = sorted_ccemb_[(std::size_t)r][(std::size_t)j];
          float newSim = (1 + sqrt((i + 1) * (j + 1)) *
                                  simEmb(embycc_[(std::size_t)k1],
                                         embycc_[(std::size_t)k2])) /
                         getPosition((unsigned int)k1, (unsigned int)k2);
          MX(k1, k2) = newSim;
          incElems_[(std::size_t)k1].insert(k2);
        }
      }
    }

    for (int q = 0; q < n_; q++) {
      for (int i = 0; i < l_; i++) {
        int img_i = rkLists_[(std::size_t)l_ * q + i];
        incElemsL_[(std::size_t)q].insert(img_i);
      }
    }

    for (int q = 0; q < n_; q++) {
      std::set<int> result;
      std::set_difference(incElems_[(std::size_t)q].cbegin(),
                          incElems_[(std::size_t)q].cend(),
                          incElemsL_[(std::size_t)q].cbegin(),
                          incElemsL_[(std::size_t)q].cend(),
                          std::inserter(result, result.cend()));
      kernel_sort_ranked_lists_extra(q, result);
    }
  }

  // ---- CC data members (legacy RFE.hpp:116-140 types; underscore suffix) ----
  // rcc_/icc_/ccemb_/embycc_ are std::map in the legacy too: the ascending-key
  // iteration of ccemb_ IS reRankingByCCs' CC order (hence its write-conflict
  // order), so a vector would be a semantic change, not a shim.
  std::vector<std::pair<float, std::pair<int, int>>> edgesCCs_;
  std::map<int, std::set<int>> rcc_;
  std::map<int, int> icc_;
  double th_cc_ = 0.0;
  std::map<int, std::map<int, float>> ccemb_;
  std::vector<std::vector<int>> sorted_ccemb_;
  std::map<int, std::map<int, float>> embycc_;
};

#endif  // RFE_REF_VERBATIM_HPP