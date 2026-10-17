#ifndef LHRR_SPARSE_CORE_HPP
#define LHRR_SPARSE_CORE_HPP

// Faithful LHRR sparse core. Execution stays serial by default; phase C can
// opt into row-parallel OpenMP. Checkpoint drivers remain under ports/LHRR.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace lhrr_sparse {

using HElem = std::pair<int, double>;
using HEList = std::vector<HElem>;
using HType = std::vector<HEList>;
using CType = std::vector<std::vector<int>>;
using CvalType = std::vector<std::vector<float>>;
using RType = std::vector<HEList>;
using ConfidType = std::vector<double>;

struct WEntry {
  int col;
  uint16_t pos;
  uint16_t weight;
};
using WRow = std::vector<WEntry>;
using WType = std::vector<WRow>;

inline WType buildW(const uint32_t* rkLists, int n, int l) {
  if (l <= 0 || l >= 65536)
    throw std::runtime_error("LHRR_SPARSE: require 0 < L < 65536 for uint16_t W fields");
  WType W(n);
  for (int rk = 0; rk < n; ++rk) {
    W[rk].reserve(l);
    for (int pos = 0; pos < l; ++pos) {
      const int img = static_cast<int>(rkLists[static_cast<long>(l) * rk + pos]);
      W[rk].push_back({img, static_cast<uint16_t>(pos),
                       static_cast<uint16_t>(l - pos)});
    }
    std::sort(W[rk].begin(), W[rk].end(),
              [](const WEntry& a, const WEntry& b) {
                if (a.col != b.col) return a.col < b.col;
                return a.pos < b.pos;
              });
  }
  return W;
}

struct WColLess {
  bool operator()(const WEntry& entry, int value) const { return entry.col < value; }
  bool operator()(int value, const WEntry& entry) const { return value < entry.col; }
};

inline void k2_sort(float* a, uint32_t* list, int l, int self_id) {
  int i;
  for (int j = 2; j < l; ++j) {
    const float keyA = a[j];
    const uint32_t keyR = list[j];
    i = j - 1;
    while (i > 0 && a[i] < keyA) {
      a[i + 1] = a[i];
      list[i + 1] = list[i];
      --i;
    }
    a[i + 1] = keyA;
    list[i + 1] = keyR;
  }
  i = 0;
  while (i < l && list[i] != static_cast<uint32_t>(self_id)) ++i;
  if (i > 0 && i < l) {
    std::swap(list[0], list[i]);
    std::swap(a[0], a[i]);
  }
}

inline float initial_sort_score(const WType& W, int rk, int img) {
  float score = 0.0f;
  const auto forward = std::equal_range(W[rk].begin(), W[rk].end(), img, WColLess{});
  for (auto it = forward.first; it != forward.second; ++it)
    score += static_cast<float>(it->weight);
  const auto backward = std::equal_range(W[img].begin(), W[img].end(), rk, WColLess{});
  for (auto it = backward.first; it != backward.second; ++it)
    score += static_cast<float>(it->weight);
  return score;
}

inline void initial_sort_rankings(const WType& W, uint32_t* rkLists,
                                  int n, int l) {
  std::vector<float> scores(l);
  std::vector<uint32_t> list(l);
  for (int rk = 0; rk < n; ++rk) {
    for (int j = 0; j < l; ++j) {
      const int img = static_cast<int>(rkLists[static_cast<long>(l) * rk + j]);
      scores[j] = initial_sort_score(W, rk, img);
      list[j] = static_cast<uint32_t>(img);
    }
    k2_sort(scores.data(), list.data(), l, rk);
    for (int j = 0; j < l; ++j)
      rkLists[static_cast<long>(l) * rk + j] = list[j];
  }
}

inline double weight_position(int pos, int k) {
  if (pos == 1) return 1.0;
  return 1.0 - std::log(static_cast<double>(pos)) /
                    std::log(static_cast<double>(k));
}

inline void validate_input(const uint32_t* rkLists, int n, int l, int k, int t) {
  if (n <= 0) throw std::runtime_error("LHRR_SPARSE: N must be > 0");
  if (l <= 0 || l > n)
    throw std::runtime_error("LHRR_SPARSE: require 1 <= L <= N");
  if (l >= 65536)
    throw std::runtime_error("LHRR_SPARSE: require L < 65536 for uint16_t W fields");
  if (k < 2 || k > l)
    throw std::runtime_error("LHRR_SPARSE: require 2 <= K <= L");
  if (t <= 0) throw std::runtime_error("LHRR_SPARSE: T must be > 0");

  std::vector<uint32_t> row(l);
  for (int r = 0; r < n; ++r) {
    bool has_self = false;
    for (int j = 0; j < l; ++j) {
      const uint32_t value = rkLists[static_cast<long>(l) * r + j];
      if (value >= static_cast<uint32_t>(n))
        throw std::runtime_error("LHRR_SPARSE: ranking id out of range at row " +
                                 std::to_string(r));
      has_self = has_self || value == static_cast<uint32_t>(r);
    }
    // Duplicates are accepted: the reference K-b pipeline processes them as
    // repeated ranking positions (the hub fixture exercises this behavior).
    if (!has_self)
      throw std::runtime_error("LHRR_SPARSE: self missing at row " +
                               std::to_string(r));
  }
}

inline void fase_a(const uint32_t* rkLists, int n, int l, int k, HType& H,
                   CType& C, CvalType& Cval, ConfidType& confid) {
  H.resize(n);
  C.resize(n);
  Cval.resize(n);
  confid.resize(n);

  std::vector<double> tmp_h(n, 0.0);
  std::vector<float> tmp_c(n, 0.0f);
  std::vector<char> touched_h(n, 0);
  std::vector<char> touched_c(n, 0);
  std::vector<int> order_h;
  std::vector<int> order_c;

  for (int img = 0; img < n; ++img) {
    for (int idx : order_h) tmp_h[idx] = 0.0;
    for (int idx : order_c) tmp_c[idx] = 0.0f;
    order_h.clear();
    order_c.clear();
    touched_h.assign(n, 0);
    touched_c.assign(n, 0);

    tmp_h[img] = 1.0;
    touched_h[img] = 1;
    order_h.push_back(img);
    tmp_c[img] = 1.0f;
    touched_c[img] = 1;
    order_c.push_back(img);

    for (int o = 0; o < k; ++o) {
      const int imgo = static_cast<int>(rkLists[static_cast<long>(l) * img + o]);
      const double wO = weight_position(o + 1, k);
      for (int j = 0; j < k; ++j) {
        const int imgj = static_cast<int>(rkLists[static_cast<long>(l) * imgo + j]);
        const double w = wO * weight_position(j + 1, k);

        if (!touched_h[imgj]) {
          touched_h[imgj] = 1;
          order_h.push_back(imgj);
        }
        tmp_h[imgj] += w;

        const float wf = static_cast<float>(w);
        if (tmp_c[imgj] == 0.0f && wf != 0.0f) order_c.push_back(imgj);
        tmp_c[imgj] += wf;
        touched_c[imgj] = 1;
      }
    }

    H[img].clear();
    H[img].reserve(order_h.size());
    for (int idx : order_h) H[img].push_back({idx, tmp_h[idx]});
    std::sort(H[img].begin(), H[img].end(),
              [](const HElem& a, const HElem& b) { return a.second > b.second; });

    confid[img] = 0.0;
    const int k_limit = std::min(static_cast<int>(H[img].size()), k);
    for (int i = 0; i < k_limit; ++i) confid[img] += H[img][i].second;

    C[img].clear();
    Cval[img].clear();
    C[img].reserve(order_c.size());
    Cval[img].reserve(order_c.size());
    for (int idx : order_c) {
      C[img].push_back(idx);
      Cval[img].push_back(tmp_c[idx]);
    }
  }
}

inline void fase_b(const HType& H, int n, RType& R) {
  R.clear();
  R.resize(n);
  for (int img = 0; img < n; ++img) {
    for (const auto& [a, value] : H[img]) R[a].push_back({img, value});
  }
}

struct PhaseCScratch {
  std::vector<float> acc;
  std::vector<float> tmpA;
  std::vector<float> tmpB;
  std::vector<int> cand;
  std::vector<float> a1;
  std::vector<float> a2;
  std::vector<float> scratch_a;

  void init(int n, int l) {
    acc.resize(n);
    tmpA.resize(n);
    tmpB.resize(n);
    cand.reserve(l * 4);
    a1.resize(l);
    a2.reserve(l * 4);
    scratch_a.resize(l);
  }
};

inline std::vector<uint32_t> k9_join_linear(
    const std::vector<uint32_t>& list_work,
    const std::vector<int>& candidates,
    const std::vector<float>& a1,
    const std::vector<float>& a2) {
  const int l = static_cast<int>(list_work.size());
  if (candidates.empty()) return list_work;
  std::vector<uint32_t> output(static_cast<size_t>(l));
  output[0] = list_work[0];
  size_t candidate = 0;
  int shift = 0;
  for (int i = 1; i < l; ++i) {
    if (candidate < candidates.size() && a2[candidate] > a1[i]) {
      output[i] = static_cast<uint32_t>(candidates[candidate]);
      ++candidate;
      ++shift;
    } else {
      output[i] = list_work[static_cast<size_t>(i - shift)];
    }
  }
  return output;
}

inline void phase_c_row(int q, const HType& H, const CType& C,
                        const CvalType& Cval, const RType& R,
                        const ConfidType& confid, const uint32_t* old_lists,
                        uint32_t* new_lists, int n, int l, int k,
                        PhaseCScratch& scratch) {
  auto get_old = [&](long idx) -> uint32_t { return old_lists[idx]; };
  std::fill(scratch.acc.begin(), scratch.acc.end(), 0.0f);
  const int self_id = q;
  for (int j = 0; j < l; ++j)
    scratch.acc[get_old(static_cast<long>(l) * q + j)] = 1.0f;
  scratch.cand.clear();

  for (const auto& [o, v1] : R[q]) {
    const double conf = confid[o];
    for (const auto& [b, v2] : H[o]) {
      const double inc = (conf * v1) * v2;
      if (scratch.acc[b] == 0.0f) {
        scratch.cand.push_back(b);
        scratch.acc[b] = 1.0f;
      }
      scratch.acc[b] = static_cast<float>(static_cast<double>(scratch.acc[b]) + inc);
    }
  }

  std::fill(scratch.tmpA.begin(), scratch.tmpA.end(), 0.0f);
  for (size_t i = 0; i < C[q].size(); ++i) scratch.tmpA[C[q][i]] = Cval[q][i];
  for (int j = 1; j < l; ++j) {
    const uint32_t img = get_old(static_cast<long>(l) * q + j);
    double sim = 0.0;
    for (size_t o = 0; o < C[img].size(); ++o)
      sim += scratch.tmpA[C[img][o]] * Cval[img][o];
    scratch.acc[img] = static_cast<float>(static_cast<double>(scratch.acc[img]) * sim);
  }

  std::fill(scratch.tmpB.begin(), scratch.tmpB.end(), 0.0f);
  for (const auto& [o, v1] : R[q]) scratch.tmpB[o] = static_cast<float>(v1);
  for (int j = 1; j < l; ++j) {
    const uint32_t img = get_old(static_cast<long>(l) * q + j);
    double sim = 0.0;
    for (size_t o = 0; o < C[img].size(); ++o)
      sim += scratch.tmpB[C[img][o]] * Cval[img][o];
    scratch.acc[img] = static_cast<float>(static_cast<double>(scratch.acc[img]) * sim);
  }

  struct CandPair {
    int id;
    float val;
  };
  std::vector<CandPair> cand_pairs;
  cand_pairs.reserve(scratch.cand.size());
  for (int b : scratch.cand) cand_pairs.push_back({b, scratch.acc[b]});
  std::sort(cand_pairs.begin(), cand_pairs.end(),
            [](const CandPair& a, const CandPair& b) { return a.val > b.val; });
  scratch.cand.clear();
  for (const auto& p : cand_pairs) scratch.cand.push_back(p.id);

  for (int j = 0; j < l; ++j) {
    const uint32_t img = get_old(static_cast<long>(l) * q + j);
    scratch.scratch_a[j] = scratch.acc[img];
  }
  std::vector<uint32_t> list_work(l);
  for (int j = 0; j < l; ++j) list_work[j] = get_old(static_cast<long>(l) * q + j);
  k2_sort(scratch.scratch_a.data(), list_work.data(), l, self_id);

  for (int j = 0; j < l; ++j) scratch.a1[j] = scratch.acc[list_work[j]];
  scratch.a2.clear();
  for (int b : scratch.cand) scratch.a2.push_back(scratch.acc[b]);

  const std::vector<uint32_t> output =
      k9_join_linear(list_work, scratch.cand, scratch.a1, scratch.a2);
  for (int j = 0; j < l; ++j)
    new_lists[static_cast<long>(l) * q + j] = output[j];
}

class Pipeline {
 public:
  void init(int n_, int l_, int k_, int t_) {
    n = n_;
    l = l_;
    k = k_;
    t = t_;
    if (l <= 0 || l >= 65536)
      throw std::runtime_error("LHRR_SPARSE: require 0 < L < 65536");
    if (n <= 0 || l <= 0 || l > n || k < 2 || k > l || t <= 0)
      throw std::runtime_error("LHRR_SPARSE: invalid pipeline parameters");
    rkLists.resize(static_cast<size_t>(n) * static_cast<size_t>(l));
    rkLists_new.resize(rkLists.size());
  }

  void set_rkLists(const uint32_t* src) {
    std::copy(src, src + rkLists.size(), rkLists.begin());
  }

  void run(int requested_threads = 1) {
    if (requested_threads <= 0)
      throw std::runtime_error("LHRR_SPARSE: thread count must be > 0");
#ifndef _OPENMP
    if (requested_threads > 1)
      throw std::runtime_error("LHRR_SPARSE: multiple threads requested without OpenMP support");
#endif
    validate_input(rkLists.data(), n, l, k, t);
    const WType W = buildW(rkLists.data(), n, l);
    initial_sort(W);

    for (int iter = 1; iter <= t; ++iter) {
      HType H;
      CType C;
      CvalType Cval;
      ConfidType confid;
      fase_a(rkLists.data(), n, l, k, H, C, Cval, confid);
      for (int img = 0; img < n; ++img) {
        if (static_cast<int>(H[img].size()) < k)
          throw std::runtime_error("LHRR_SPARSE: |HE| < K at image " +
                                   std::to_string(img));
      }

      RType R;
      fase_b(H, n, R);
      run_phase_c(H, C, Cval, R, confid, requested_threads);
      rkLists.swap(rkLists_new);
    }
  }

  const std::vector<uint32_t>& get_result() const { return rkLists; }

 private:
  void run_phase_c(const HType& H, const CType& C, const CvalType& Cval,
                   const RType& R, const ConfidType& confid,
                   int requested_threads) {
#ifdef _OPENMP
    if (requested_threads > 1) {
#pragma omp parallel num_threads(requested_threads)
      {
        PhaseCScratch scratch;
        scratch.init(n, l);
#pragma omp for schedule(static)
        for (int q = 0; q < n; ++q)
          phase_c_row(q, H, C, Cval, R, confid, rkLists.data(),
                      rkLists_new.data(), n, l, k, scratch);
      }
      return;
    }
#endif
    PhaseCScratch scratch;
    scratch.init(n, l);
    for (int q = 0; q < n; ++q)
      phase_c_row(q, H, C, Cval, R, confid, rkLists.data(),
                  rkLists_new.data(), n, l, k, scratch);
  }

  void initial_sort(const WType& W) {
    initial_sort_rankings(W, rkLists.data(), n, l);
  }

  int n = 0;
  int l = 0;
  int k = 0;
  int t = 0;
  std::vector<uint32_t> rkLists;
  std::vector<uint32_t> rkLists_new;
};

}  // namespace lhrr_sparse

#endif  // LHRR_SPARSE_CORE_HPP