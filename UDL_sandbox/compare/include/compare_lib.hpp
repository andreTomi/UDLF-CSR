// compare_lib.hpp — T2: header-only comparison + D0–D4 divergence classifier.
// Conventions (docs/DIVERGENCE_CLASSES.md):
//   precedence D4 > D3 > D2 > D1 > D0; "tie" = values exactly equal in the ORIGINAL.
//   ULP distance = classic ordered-integer mapping of the raw bits:
//     ord(b) = sign_set ? ~b : b | sign   (32-bit for float, 64-bit for double);
//     dist   = |ord(a) - ord(b)| (unsigned). -0.0 vs +0.0 -> dist 1.
//   ints are exact: any difference is beyond N_ULP (-> D2), max_ulp not applicable ("-").
//   non-finite: NaN/Inf in CANDIDATE where ORIGINAL is finite -> D4. Other non-finite
//   mismatches count as beyond-ULP diffs -> D2 (documented in README).
//   Empty input (0 items) -> std::runtime_error (CLI exits 2).
#ifndef UDL_COMPARE_LIB_HPP
#define UDL_COMPARE_LIB_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cmp {

enum class Class { D0, D1, D2, D3, D4 };

inline const char* className(Class c) {
  switch (c) {
    case Class::D0: return "D0";
    case Class::D1: return "D1";
    case Class::D2: return "D2";
    case Class::D3: return "D3";
    case Class::D4: return "D4";
  }
  return "?";
}

struct Result {
  Class cls = Class::D0;
  size_t items = 0;
  size_t diffs = 0;               // value/bit differences (dense & sparse common cells)
  unsigned long long maxUlp = 0;  // meaningful iff maxUlpValid
  bool maxUlpValid = false;
  size_t missing = 0;             // support cells/ids present in ORIGINAL, absent in CAND
  size_t extra = 0;               // support cells/ids present in CAND, absent in ORIGINAL
  size_t rankPosDiff = 0;         // (row, position) with different id
  size_t tieOnly = 0;             // rankPosDiff explained by exact ties of the ORIGINAL
  size_t zerosOrig = 0;           // cells with value exactly 0 (dense/sparse)
  size_t zerosCand = 0;
  std::string reason;             // appended to the output line when non-empty
};

struct Options {
  int nUlp = 4;  // PROVISÓRIO (pendente de confirmação em DECISIONS.md)
};

// ---------------------------------------------------------------------------
// bit-level helpers
// ---------------------------------------------------------------------------
template <class T>
inline bool bitEq(T a, T b) {
  uint64_t ba = 0, bb = 0;
  std::memcpy(&ba, &a, sizeof(T));
  std::memcpy(&bb, &b, sizeof(T));
  return ba == bb;
}

inline uint64_t ord32(uint32_t b) {
  return (b & 0x80000000u) ? (uint64_t)(~b) : (uint64_t)b + 0x80000000ull;
}
inline uint64_t ord64(uint64_t b) {
  return (b & 0x8000000000000000ull) ? ~b : b + 0x8000000000000000ull;
}

inline unsigned long long ulpDist(float a, float b) {
  uint32_t ba, bb;
  std::memcpy(&ba, &a, 4);
  std::memcpy(&bb, &b, 4);
  uint64_t x = ord32(ba), y = ord32(bb);
  return (unsigned long long)(x > y ? x - y : y - x);
}
inline unsigned long long ulpDist(double a, double b) {
  uint64_t ba, bb;
  std::memcpy(&ba, &a, 8);
  std::memcpy(&bb, &b, 8);
  uint64_t x = ord64(ba), y = ord64(bb);
  return x > y ? x - y : y - x;
}
inline unsigned long long ulpDist(int32_t a, int32_t b) {
  return a == b ? 0ull : std::numeric_limits<unsigned long long>::max();
}

template <class T> inline bool finiteQ(T v);
template <> inline bool finiteQ<float>(float v) { return std::isfinite(v); }
template <> inline bool finiteQ<double>(double v) { return std::isfinite(v); }
template <> inline bool finiteQ<int32_t>(int32_t) { return true; }

template <class T> inline bool isZero(T v) { return v == T(0); }  // true for -0 too

template <class T> struct IsFloat : std::false_type {};
template <> struct IsFloat<float> : std::true_type {};
template <> struct IsFloat<double> : std::true_type {};

template <class T> inline T parseVal(const std::string& s);
template <> inline float parseVal<float>(const std::string& s) {
  size_t p = 0;
  float v = std::stof(s, &p);
  if (p != s.size()) throw std::runtime_error("bad float token: " + s);
  return v;
}
template <> inline double parseVal<double>(const std::string& s) {
  size_t p = 0;
  double v = std::stod(s, &p);
  if (p != s.size()) throw std::runtime_error("bad double token: " + s);
  return v;
}
template <> inline int32_t parseVal<int32_t>(const std::string& s) {
  size_t p = 0;
  long v = std::stol(s, &p);
  if (p != s.size() || v < std::numeric_limits<int32_t>::min() ||
      v > std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("bad int token: " + s);
  }
  return (int32_t)v;
}

// ---------------------------------------------------------------------------
// data structures
// ---------------------------------------------------------------------------
template <class T>
struct DenseMat {
  size_t rows = 0, cols = 0;
  std::vector<T> v;
  size_t items() const { return v.size(); }
};

template <class T>
struct SparseMat {
  size_t rows = 0, cols = 0;
  struct Trip { size_t r, c; T v; };
  std::vector<Trip> trips;
  size_t items() const { return trips.size(); }
};

struct RankMat {
  size_t n = 0, L = 0;
  std::vector<int32_t> v;
  size_t items() const { return v.size(); }
  int32_t at(size_t r, size_t j) const { return v[r * L + j]; }
};

// ---------------------------------------------------------------------------
// loaders (strict: any problem -> std::runtime_error; 0 items -> "empty" error)
// ---------------------------------------------------------------------------
inline void ensureNotEmpty(size_t items, const std::string& what) {
  if (items == 0) {
    throw std::runtime_error("empty comparison: " + what + " has 0 items");
  }
}

template <class T>
DenseMat<T> loadDense(std::istream& in, const std::string& label) {
  DenseMat<T> m;
  if (!(in >> m.rows >> m.cols)) {
    throw std::runtime_error("invalid/empty dense header (rows cols) in " + label);
  }
  m.v.reserve(m.rows * m.cols);
  for (size_t i = 0; i < m.rows * m.cols; ++i) {
    std::string tok;
    if (!(in >> tok)) {
      throw std::runtime_error("dense file truncated (" + label + "): expected " +
                               std::to_string(m.rows * m.cols) + " values");
    }
    m.v.push_back(parseVal<T>(tok));
  }
  std::string extra;
  if (in >> extra) {
    throw std::runtime_error("dense file has extra data after the matrix (" + label + ")");
  }
  ensureNotEmpty(m.items(), "dense " + label);
  return m;
}

template <class T>
SparseMat<T> loadSparse(std::istream& in, const std::string& label) {
  SparseMat<T> m;
  if (!(in >> m.rows >> m.cols)) {
    throw std::runtime_error("invalid/empty sparse header (rows cols) in " + label);
  }
  std::map<uint64_t, int> seen;
  while (true) {
    std::string rs;
    if (!(in >> rs)) break;
    std::string cs, vs;
    if (!(in >> cs >> vs)) {
      throw std::runtime_error("sparse file truncated (row col value) in " + label);
    }
    long r = std::stol(rs), c = std::stol(cs);
    if (r < 0 || (size_t)r >= m.rows || c < 0 || (size_t)c >= m.cols) {
      throw std::runtime_error("sparse cell out of bounds in " + label + ": (" + rs + "," + cs + ")");
    }
    uint64_t key = ((uint64_t)r << 32) | (uint64_t)c;
    if (seen.count(key)) {
      throw std::runtime_error("duplicate sparse cell (" + rs + "," + cs + ") in " + label);
    }
    seen[key] = 1;
    m.trips.push_back({(size_t)r, (size_t)c, parseVal<T>(vs)});
  }
  ensureNotEmpty(m.items(), "sparse " + label);
  return m;
}

inline RankMat loadRank(std::istream& in, const std::string& label) {
  RankMat m;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      throw std::runtime_error("rank file has an empty line in " + label);
    }
    std::istringstream ls(line);
    std::string tok;
    size_t j = 0;
    while (ls >> tok) {
      int32_t val = parseVal<int32_t>(tok);
      if (m.n == 0 && j == 0) m.L = 0;  // L grows with first row
      if (m.n > 0 && j >= m.L) {
        throw std::runtime_error("rank row wider than L in " + label);
      }
      if (m.n == 0) m.L = j + 1;
      if (m.n > 0 && j == 0 && m.L == 0) m.L = 1;
      m.v.push_back(val);
      ++j;
    }
    if (m.n > 0 && j != m.L) {
      throw std::runtime_error("rank row " + std::to_string(m.n) + " has " +
                               std::to_string(j) + " tokens, expected " +
                               std::to_string(m.L) + " (" + label + ")");
    }
    ++m.n;
  }
  ensureNotEmpty(m.items(), "rank " + label);
  return m;
}

template <class T>
DenseMat<T> loadDenseFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  return loadDense<T>(f, path);
}
template <class T>
SparseMat<T> loadSparseFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  return loadSparse<T>(f, path);
}
inline RankMat loadRankFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  return loadRank(f, path);
}

// ---------------------------------------------------------------------------
// comparators
// ---------------------------------------------------------------------------
template <class T>
bool vecBitEq(const std::vector<T>& a, const std::vector<T>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!bitEq(a[i], b[i])) return false;
  }
  return true;
}

template <class T>
Result compareDense(const DenseMat<T>& o, const DenseMat<T>& c,
                    const DenseMat<T>* input, const Options& op) {
  Result r;
  ensureNotEmpty(o.items(), "dense original");
  ensureNotEmpty(c.items(), "dense candidate");
  if (input) ensureNotEmpty(input->items(), "dense input");
  if (o.rows != c.rows || o.cols != c.cols) {
    r.cls = Class::D4;
    r.reason = "dim_mismatch";
    return r;
  }
  if (input) {
    if (vecBitEq(c.v, input->v) && !vecBitEq(o.v, input->v)) {
      r.cls = Class::D4;
      r.reason = "inert";
      return r;
    }
  }
  for (size_t i = 0; i < o.v.size(); ++i) {
    if (finiteQ(o.v[i]) && !finiteQ(c.v[i])) {
      r.cls = Class::D4;
      r.reason = "nan_inf_in_cand";
      return r;
    }
  }
  r.items = o.v.size();
  for (size_t i = 0; i < o.v.size(); ++i) {
    if (isZero(o.v[i])) ++r.zerosOrig;
    if (isZero(c.v[i])) ++r.zerosCand;
    if (bitEq(o.v[i], c.v[i])) continue;
    ++r.diffs;
    unsigned long long d;
    if (!finiteQ(o.v[i]) || !finiteQ(c.v[i])) {
      d = std::numeric_limits<unsigned long long>::max();  // non-finite mismatch -> beyond
    } else {
      d = ulpDist(o.v[i], c.v[i]);
    }
    r.maxUlp = std::max(r.maxUlp, d);
  }
  if (r.diffs == 0) {
    r.cls = Class::D0;
  } else {
    r.maxUlpValid = IsFloat<T>::value;
    if (r.maxUlp <= (unsigned long long)op.nUlp && IsFloat<T>::value) {
      r.cls = Class::D1;
    } else {
      r.cls = Class::D2;
      r.reason = (r.maxUlp > (unsigned long long)op.nUlp) ? "beyond_nulp" : "int_exact";
    }
  }
  return r;
}

template <class T>
Result compareDense(const DenseMat<T>& o, const DenseMat<T>& c, const Options& op) {
  return compareDense<T>(o, c, nullptr, op);
}
template <class T>
Result compareDense(const DenseMat<T>& o, const DenseMat<T>& c, std::nullptr_t,
                    const Options& op) {
  return compareDense<T>(o, c, static_cast<const DenseMat<T>*>(nullptr), op);
}

template <class T>
std::map<uint64_t, T> sparseMap(const SparseMat<T>& m) {
  std::map<uint64_t, T> mp;
  for (const auto& t : m.trips) {
    mp[((uint64_t)t.r << 32) | (uint64_t)t.c] = t.v;
  }
  return mp;
}

template <class T>
Result compareSparse(const SparseMat<T>& o, const SparseMat<T>& c,
                     const SparseMat<T>* input, const Options& op) {
  Result r;
  ensureNotEmpty(o.items(), "sparse original");
  ensureNotEmpty(c.items(), "sparse candidate");
  if (input) ensureNotEmpty(input->items(), "sparse input");
  if (o.rows != c.rows || o.cols != c.cols) {
    r.cls = Class::D4;
    r.reason = "dim_mismatch";
    return r;
  }
  if (input) {
    auto mi = sparseMap(*input), mc = sparseMap(c), mo = sparseMap(o);
    if (mc == mi && mo != mi) {
      r.cls = Class::D4;
      r.reason = "inert";
      return r;
    }
  }
  auto mo = sparseMap(o), mc = sparseMap(c);
  for (const auto& t : c.trips) {
    if (!finiteQ(t.v)) {
      auto it = mo.find(((uint64_t)t.r << 32) | (uint64_t)t.c);
      if (it != mo.end() && finiteQ(it->second)) {
        r.cls = Class::D4;
        r.reason = "nan_inf_in_cand";
        return r;
      }
    }
  }
  r.items = o.items() + c.items();  // total support entries inspected
  for (const auto& kv : mo) {
    if (isZero(kv.second)) ++r.zerosOrig;
    auto it = mc.find(kv.first);
    if (it == mc.end()) {
      ++r.missing;
      continue;
    }
    if (bitEq(kv.second, it->second)) continue;
    ++r.diffs;
    unsigned long long d =
        (!finiteQ(kv.second) || !finiteQ(it->second))
            ? std::numeric_limits<unsigned long long>::max()
            : ulpDist(kv.second, it->second);
    r.maxUlp = std::max(r.maxUlp, d);
  }
  for (const auto& kv : mc) {
    if (isZero(kv.second)) ++r.zerosCand;
    if (!mo.count(kv.first)) ++r.extra;
  }
  if (r.missing || r.extra) {
    r.cls = Class::D3;
    r.reason = "support_diff";
  } else if (r.diffs == 0) {
    r.cls = Class::D0;
  } else {
    r.maxUlpValid = IsFloat<T>::value;
    if (r.maxUlp <= (unsigned long long)op.nUlp && IsFloat<T>::value) {
      r.cls = Class::D1;
    } else {
      r.cls = Class::D2;
      r.reason = "beyond_nulp";
    }
  }
  return r;
}

template <class T>
Result compareSparse(const SparseMat<T>& o, const SparseMat<T>& c, const Options& op) {
  return compareSparse<T>(o, c, nullptr, op);
}
template <class T>
Result compareSparse(const SparseMat<T>& o, const SparseMat<T>& c, std::nullptr_t,
                     const Options& op) {
  return compareSparse<T>(o, c, static_cast<const SparseMat<T>*>(nullptr), op);
}

// Rank comparison. Ties (decision humana, T2): scores (n x L, aligned to the
// ORIGINAL list, doubles so float/double kernels round-trip exactly) are
// OPTIONAL; without scores any position difference is conservatively D2.
// A differing row is "tie-explained" iff for every position j:
//   score_of_element(cand[r][j]) == scores[r][j]  (score looked up from ORIG)
Result compareRank(const RankMat& o, const RankMat& c, const RankMat* input,
                   const DenseMat<double>* scores, const Options& op) {
  (void)op;  // rank ids are exact; N_ULP does not apply
  Result r;
  ensureNotEmpty(o.items(), "rank original");
  ensureNotEmpty(c.items(), "rank candidate");
  if (input) ensureNotEmpty(input->items(), "rank input");
  if (o.n != c.n || o.L != c.L) {
    r.cls = Class::D4;
    r.reason = "dim_mismatch";
    return r;
  }
  if (input) {
    if (c.v == input->v && o.v != input->v) {
      r.cls = Class::D4;
      r.reason = "inert";
      return r;
    }
  }
  for (size_t i = 0; i < c.v.size(); ++i) {
    if (c.v[i] < 0 || (size_t)c.v[i] >= o.n) {
      r.cls = Class::D4;
      r.reason = "invalid_id_in_cand";
      return r;
    }
  }
  if (scores && (scores->rows != o.n || scores->cols != o.L)) {
    throw std::runtime_error("scores shape differs from the original rank shape");
  }
  r.items = o.items();
  std::vector<bool> rowTieOk(o.n, true);
  for (size_t row = 0; row < o.n; ++row) {
    std::map<int32_t, double> scoreOf;  // id -> score, from ORIGINAL row
    if (scores) {
      for (size_t j = 0; j < o.L; ++j) {
        scoreOf.emplace(o.at(row, j), scores->v[row * scores->cols + j]);
      }
    }
    // multiset support of the row
    std::map<int32_t, int> multiset;
    for (size_t j = 0; j < o.L; ++j) multiset[o.at(row, j)]++;
    for (size_t j = 0; j < o.L; ++j) multiset[c.at(row, j)]--;
    for (const auto& kv : multiset) {
      if (kv.second > 0) r.missing += (size_t)kv.second;
      if (kv.second < 0) r.extra += (size_t)(-kv.second);
    }
    bool rowDiff = false;
    for (size_t j = 0; j < o.L; ++j) {
      if (o.at(row, j) == c.at(row, j)) continue;
      rowDiff = true;
      ++r.rankPosDiff;
    }
    if (rowDiff && scores) {
      for (size_t j = 0; j < o.L; ++j) {
        auto it = scoreOf.find(c.at(row, j));
        if (it == scoreOf.end() ||
            !bitEq(it->second, scores->v[row * scores->cols + j])) {
          rowTieOk[row] = false;  // ordering beyond exact ties changed
          break;
        }
      }
      if (rowTieOk[row]) r.tieOnly += /* pos diffs in this row */ [&] {
        size_t d = 0;
        for (size_t j = 0; j < o.L; ++j) d += (o.at(row, j) != c.at(row, j));
        return d;
      }();
    }
  }
  if (r.missing || r.extra) {
    r.cls = Class::D3;
    r.reason = "support_diff";
  } else if (r.rankPosDiff == 0) {
    r.cls = Class::D0;
  } else {
    if (scores) {
      bool allTie = true;
      for (size_t row = 0; row < o.n; ++row) {
        if (!rowTieOk[row]) { allTie = false; break; }
      }
      if (allTie) {
        r.cls = Class::D1;
        r.reason = "order_within_ties";
      } else {
        r.cls = Class::D2;
        r.reason = "order_beyond_ties";
      }
    } else {
      r.cls = Class::D2;
      r.reason = "no_scores_conservative";
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// report line (greppable, repo style: METRIC,... / SHIM_EVAL ...)
// ---------------------------------------------------------------------------
inline std::string reportLine(const std::string& fixture, const Result& r) {
  std::ostringstream os;
  os << "COMPARE," << fixture
     << ",class=" << className(r.cls)
     << ",items=" << r.items
     << ",diffs=" << r.diffs
     << ",max_ulp=";
  if (r.maxUlpValid) {
    os << r.maxUlp;
  } else {
    os << "-";
  }
  os << ",missing=" << r.missing
     << ",extra=" << r.extra
     << ",rank_pos_diff=" << r.rankPosDiff
     << ",tie_only=" << r.tieOnly
     << ",zeros_orig=" << r.zerosOrig
     << ",zeros_cand=" << r.zerosCand;
  if (!r.reason.empty()) os << ",reason=" << r.reason;
  return os.str();
}

}  // namespace cmp
#endif  // UDL_COMPARE_LIB_HPP
