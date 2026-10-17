// snapshot.hpp — T3: golden snapshot format (SNP1): binary little-endian payload
// + sidecar manifest. Dump/load for dense / canonical triples (explicit zeros
// preserved) / ranking / csr. Round-trip is verified with the T2 comparator
// (compare_lib.hpp): dump -> load -> compare against the source MUST be D0.
// Hashes: FNV-1a-64 with the STANDARD basis 14695981039346656037 (different
// from the RFERef ck basis -- documented in FORMAT.md).
#ifndef UDL_SNAPSHOT_HPP
#define UDL_SNAPSHOT_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "compare_lib.hpp"  // reused by decoders (round-trip proven against DenseMat/Triples/RankMat)

namespace snap {

enum class Layout { Dense = 1, Triples = 2, Ranking = 3, Csr = 4 };
enum class Dtype { F32 = 1, F64 = 2, I32 = 3 };

inline std::string layoutName(Layout l) {
  switch (l) {
    case Layout::Dense: return "dense";
    case Layout::Triples: return "triples";
    case Layout::Ranking: return "ranking";
    case Layout::Csr: return "csr";
  }
  return "?";
}
inline std::string dtypeName(Dtype d) {
  switch (d) {
    case Dtype::F32: return "float32";
    case Dtype::F64: return "float64";
    case Dtype::I32: return "int32";
  }
  return "?";
}

// FNV-1a-64, standard basis (snapshot integrity, NOT the RFERef ck basis).
inline std::uint64_t fnv1a64(const void* p, std::size_t nbytes) {
  const std::uint8_t* b = static_cast<const std::uint8_t*>(p);
  std::uint64_t h = 14695981039346656037ULL;
  for (std::size_t i = 0; i < nbytes; ++i) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return h;
}
inline std::string hexU64(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)v);
  return buf;
}
inline std::uint64_t parseHexU64(const std::string& s) {
  return std::strtoull(s.c_str(), nullptr, 16);
}

// FNV of a file's bytes (used for orig_src_hash / binary_hash staleness).
inline std::uint64_t fileFnv64(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::uint64_t h = 14695981039346656037ULL;
  std::vector<char> buf(65536);
  while (f.good()) {
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::streamsize got = f.gcount();
    for (std::streamsize i = 0; i < got; ++i) {
      h ^= static_cast<std::uint8_t>(buf[static_cast<size_t>(i)]);
      h *= 1099511628211ULL;
    }
    if (got < static_cast<std::streamsize>(buf.size())) break;
  }
  return h;
}
inline std::uint64_t filesFnv64(const std::vector<std::string>& paths) {
  std::uint64_t h = 14695981039346656037ULL;
  for (const auto& p : paths) {
    std::uint64_t fh = fileFnv64(p);
    for (int i = 0; i < 8; ++i) {
      h ^= (std::uint8_t)(fh >> (8 * i));
      h *= 1099511628211ULL;
    }
  }
  return h;
}

struct Manifest {
  std::string layout = "dense";
  std::string dtype = "float32";
  std::string shape = "0x0";
  std::uint64_t nnz = 0;
  std::string content_hash;   // filled by dump
  std::string orig_src_hash;  // filled by dump (filesFnv64 of orig_src_files)
  std::string binary_hash;    // filled by caller (or empty)
  std::string binary_path;    // path used for binary_hash (for re-verification)
  std::string build_flags;
  std::string method;
  std::string fixture;
  std::string point;
  std::string params;         // canonical "K=10;T=2;..."
  std::string created_utc;
  std::string orig_src_files; // semicolon-separated, relative to repo root
};

inline std::string manifestJson(const Manifest& m) {
  std::ostringstream os;
  os << "{\n"
     << "  \"format\": \"SNP1\",\n"
     << "  \"layout\": \"" << m.layout << "\",\n"
     << "  \"dtype\": \"" << m.dtype << "\",\n"
     << "  \"shape\": \"" << m.shape << "\",\n"
     << "  \"nnz\": \"" << m.nnz << "\",\n"
     << "  \"content_hash\": \"" << m.content_hash << "\",\n"
     << "  \"orig_src_hash\": \"" << m.orig_src_hash << "\",\n"
     << "  \"binary_hash\": \"" << m.binary_hash << "\",\n"
     << "  \"binary_path\": \"" << m.binary_path << "\",\n"
     << "  \"build_flags\": \"" << m.build_flags << "\",\n"
     << "  \"method\": \"" << m.method << "\",\n"
     << "  \"fixture\": \"" << m.fixture << "\",\n"
     << "  \"point\": \"" << m.point << "\",\n"
     << "  \"params\": \"" << m.params << "\",\n"
     << "  \"created_utc\": \"" << m.created_utc << "\",\n"
     << "  \"orig_src_files\": \"" << m.orig_src_files << "\"\n"
     << "}\n";
  return os.str();
}

inline Manifest defaultManifest() {
  Manifest m;
  std::time_t t = std::time(nullptr);
  std::tm* g = std::gmtime(&t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", g);
  m.created_utc = buf;
  return m;
}

// ---------------------------------------------------------------------------
// little-endian codec
// ---------------------------------------------------------------------------
inline void putU(std::vector<std::uint8_t>& b, std::uint64_t x, int nbytes) {
  for (int i = 0; i < nbytes; ++i) {
    b.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
  }
}
inline std::uint64_t getU(const std::vector<std::uint8_t>& b, size_t& pos, int nbytes) {
  if (pos + (size_t)nbytes > b.size()) throw std::runtime_error("snapshot truncated");
  std::uint64_t x = 0;
  for (int i = 0; i < nbytes; ++i) {
    x |= (std::uint64_t)b[pos + (size_t)i] << (8 * i);
  }
  pos += (size_t)nbytes;
  return x;
}
template <class T>
inline void putVal(std::vector<std::uint8_t>& b, T v) {
  static_assert(sizeof(T) <= 8, "value too wide");
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(T));
  putU(b, bits, (int)sizeof(T));
}
template <class T>
inline T getVal(const std::vector<std::uint8_t>& b, size_t& pos) {
  std::uint64_t bits = getU(b, pos, (int)sizeof(T));
  T v;
  std::memcpy(&v, &bits, sizeof(T));
  return v;
}

inline std::string sidecarPath(const std::string& snpPath) {
  if (snpPath.size() >= 4 && snpPath.compare(snpPath.size() - 4, 4, ".snp") == 0) {
    return snpPath.substr(0, snpPath.size() - 4) + ".json";
  }
  return snpPath + ".json";
}

// Writes header+payload+trailer(hash) and the sidecar manifest JSON.
inline void writeSnapshot(const std::string& path, Layout layout, Dtype dtype,
                          std::uint64_t rows, std::uint64_t cols,
                          std::vector<std::uint8_t>& payload, Manifest m) {
  std::vector<std::uint8_t> b;
  b.insert(b.end(), {'S', 'N', 'P', '1'});
  putU(b, 1, 4);                       // version
  b.push_back((std::uint8_t)layout);
  b.push_back((std::uint8_t)dtype);
  putU(b, 0, 2);                       // reserved
  putU(b, rows, 8);
  putU(b, cols, 8);
  putU(b, m.nnz, 8);                   // count: nnz (triples/csr) or rows*cols / n*L
  b.insert(b.end(), payload.begin(), payload.end());
  std::uint64_t content = fnv1a64(b.data(), b.size());
  putU(b, content, 8);

  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
  f.close();

  m.layout = layoutName(layout);
  m.dtype = dtypeName(dtype);
  m.shape = std::to_string(rows) + "x" + std::to_string(cols);
  m.content_hash = hexU64(content);
  std::ofstream j(sidecarPath(path));
  if (!j) throw std::runtime_error("cannot write " + sidecarPath(path));
  j << manifestJson(m);
}

template <class T> struct DtypeOf;
template <> struct DtypeOf<float> { static constexpr Dtype value = Dtype::F32; };
template <> struct DtypeOf<double> { static constexpr Dtype value = Dtype::F64; };
template <> struct DtypeOf<std::int32_t> { static constexpr Dtype value = Dtype::I32; };

template <class T>
void dumpDense(const std::string& path, std::uint64_t rows, std::uint64_t cols,
               const T* data, Manifest m) {
  std::vector<std::uint8_t> payload;
  payload.reserve((size_t)(rows * cols * sizeof(T)));
  for (std::uint64_t i = 0; i < rows * cols; ++i) putVal(payload, data[i]);
  m.nnz = rows * cols;
  writeSnapshot(path, Layout::Dense, DtypeOf<T>::value, rows, cols, payload, m);
}

template <class T>
struct Trp { std::uint32_t r, c; T v; };

// Canonical: sorted by (row, col), strictly increasing (duplicates rejected),
// explicit zeros preserved as-is.
template <class T>
void dumpTriples(const std::string& path, std::uint64_t rows, std::uint64_t cols,
                 std::vector<Trp<T>> trips, Manifest m) {
  std::sort(trips.begin(), trips.end(), [](const Trp<T>& a, const Trp<T>& b) {
    return a.r != b.r ? a.r < b.r : a.c < b.c;
  });
  for (size_t i = 1; i < trips.size(); ++i) {
    if (trips[i].r == trips[i - 1].r && trips[i].c == trips[i - 1].c) {
      throw std::runtime_error("duplicate cell in triples (" +
                               std::to_string(trips[i].r) + "," +
                               std::to_string(trips[i].c) + ")");
    }
  }
  std::vector<std::uint8_t> payload;
  for (const auto& t : trips) {
    putU(payload, t.r, 4);
    putU(payload, t.c, 4);
    putVal(payload, t.v);
  }
  m.nnz = trips.size();
  writeSnapshot(path, Layout::Triples, DtypeOf<T>::value, rows, cols, payload, m);
}

inline void dumpRanking(const std::string& path, std::uint64_t n, std::uint64_t L,
                        const std::int32_t* data, Manifest m) {
  std::vector<std::uint8_t> payload;
  payload.reserve((size_t)(n * L * 4));
  for (std::uint64_t i = 0; i < n * L; ++i) putVal(payload, data[i]);
  m.nnz = n * L;
  writeSnapshot(path, Layout::Ranking, Dtype::I32, n, L, payload, m);
}

template <class T>
void dumpCsr(const std::string& path, std::uint64_t rows, std::uint64_t cols,
             const std::vector<std::uint64_t>& rowPtr,
             const std::vector<std::uint32_t>& colIdx, const T* values,
             std::uint64_t nnz, Manifest m) {
  if (rowPtr.size() != rows + 1 || colIdx.size() != nnz) {
    throw std::runtime_error("csr shape mismatch");
  }
  std::vector<std::uint8_t> payload;
  for (size_t i = 0; i < rowPtr.size(); ++i) putU(payload, rowPtr[i], 8);
  for (size_t i = 0; i < colIdx.size(); ++i) putU(payload, colIdx[i], 4);
  for (std::uint64_t i = 0; i < nnz; ++i) putVal(payload, values[i]);
  m.nnz = nnz;
  writeSnapshot(path, Layout::Csr, DtypeOf<T>::value, rows, cols, payload, m);
}

// ---------------------------------------------------------------------------
// load (verifies the content-hash trailer BEFORE returning)
// ---------------------------------------------------------------------------
struct Snapshot {
  Layout layout;
  Dtype dtype;
  std::uint64_t rows = 0, cols = 0, count = 0;
  std::uint64_t contentHash = 0;
  std::vector<std::uint8_t> payload;
};

inline std::vector<std::uint8_t> readFileBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

inline Snapshot loadSnapshot(const std::string& path) {
  auto b = readFileBytes(path);
  if (b.size() < 36 + 8) throw std::runtime_error("snapshot too small: " + path);
  if (b[0] != 'S' || b[1] != 'N' || b[2] != 'P' || b[3] != '1') {
    throw std::runtime_error("bad magic in " + path);
  }
  size_t hashPos = b.size() - 8;
  std::uint64_t stored = 0;
  for (int i = 0; i < 8; ++i) stored |= (std::uint64_t)b[hashPos + (size_t)i] << (8 * i);
  std::uint64_t actual = fnv1a64(b.data(), hashPos);
  if (stored != actual) {
    throw std::runtime_error("content hash mismatch (corrupt/tampered): " + path);
  }
  Snapshot s;
  size_t pos = 4;
  std::uint64_t version = getU(b, pos, 4);
  if (version != 1) throw std::runtime_error("unsupported snapshot version");
  s.layout = (Layout)getU(b, pos, 1);
  s.dtype = (Dtype)getU(b, pos, 1);
  (void)getU(b, pos, 2);  // reserved
  s.rows = getU(b, pos, 8);
  s.cols = getU(b, pos, 8);
  s.count = getU(b, pos, 8);
  s.payload.assign(b.begin() + (std::ptrdiff_t)pos, b.begin() + (std::ptrdiff_t)hashPos);
  s.contentHash = actual;
  return s;
}

inline std::string jsonField(const std::string& text, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t k = text.find(pat);
  if (k == std::string::npos) return "";
  size_t colon = text.find(':', k + pat.size());
  if (colon == std::string::npos) return "";
  size_t q1 = text.find('"', colon + 1);
  if (q1 == std::string::npos) return "";
  size_t q2 = text.find('"', q1 + 1);
  if (q2 == std::string::npos) return "";
  return text.substr(q1 + 1, q2 - q1 - 1);
}

inline std::string loadManifestJson(const std::string& snpPath) {
  std::ifstream f(sidecarPath(snpPath));
  if (!f) throw std::runtime_error("manifest not found: " + sidecarPath(snpPath));
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// decoders into the T2 types (round-trip verified with the T2 comparator)
// ---------------------------------------------------------------------------
template <class T>
cmp::DenseMat<T> decodeDense(const Snapshot& s) {
  if (s.layout != Layout::Dense) throw std::runtime_error("not a dense snapshot");
  if (s.dtype != DtypeOf<T>::value) throw std::runtime_error("dense dtype mismatch");
  cmp::DenseMat<T> m;
  m.rows = (size_t)s.rows;
  m.cols = (size_t)s.cols;
  size_t pos = 0;
  m.v.reserve((size_t)s.count);
  for (std::uint64_t i = 0; i < s.count; ++i) m.v.push_back(getVal<T>(s.payload, pos));
  if (pos != s.payload.size()) throw std::runtime_error("dense payload size mismatch");
  return m;
}

template <class T>
cmp::SparseMat<T> decodeTriples(const Snapshot& s) {
  if (s.layout != Layout::Triples) throw std::runtime_error("not a triples snapshot");
  if (s.dtype != DtypeOf<T>::value) throw std::runtime_error("triples dtype mismatch");
  cmp::SparseMat<T> m;
  m.rows = (size_t)s.rows;
  m.cols = (size_t)s.cols;
  size_t pos = 0;
  for (std::uint64_t i = 0; i < s.count; ++i) {
    std::uint64_t r = getU(s.payload, pos, 4);
    std::uint64_t c = getU(s.payload, pos, 4);
    T v = getVal<T>(s.payload, pos);
    m.trips.push_back({(size_t)r, (size_t)c, v});
  }
  if (pos != s.payload.size()) throw std::runtime_error("triples payload size mismatch");
  return m;
}

inline cmp::RankMat decodeRanking(const Snapshot& s) {
  if (s.layout != Layout::Ranking) throw std::runtime_error("not a ranking snapshot");
  if (s.dtype != Dtype::I32) throw std::runtime_error("ranking dtype mismatch");
  cmp::RankMat m;
  m.n = (size_t)s.rows;
  m.L = (size_t)s.cols;
  size_t pos = 0;
  m.v.reserve((size_t)s.count);
  for (std::uint64_t i = 0; i < s.count; ++i) {
    m.v.push_back(getVal<std::int32_t>(s.payload, pos));
  }
  if (pos != s.payload.size()) throw std::runtime_error("ranking payload size mismatch");
  return m;
}

// CSR -> triples (every stored entry is a cell; explicit zeros are kept).
template <class T>
cmp::SparseMat<T> decodeCsrAsTriples(const Snapshot& s) {
  if (s.layout != Layout::Csr) throw std::runtime_error("not a csr snapshot");
  if (s.dtype != DtypeOf<T>::value) throw std::runtime_error("csr dtype mismatch");
  cmp::SparseMat<T> m;
  m.rows = (size_t)s.rows;
  m.cols = (size_t)s.cols;
  size_t pos = 0;
  std::vector<std::uint64_t> rowPtr;
  for (size_t i = 0; i < (size_t)s.rows + 1; ++i) rowPtr.push_back(getU(s.payload, pos, 8));
  std::vector<std::uint32_t> colIdx;
  for (std::uint64_t i = 0; i < s.count; ++i) {
    colIdx.push_back((std::uint32_t)getU(s.payload, pos, 4));
  }
  std::vector<T> values;
  for (std::uint64_t i = 0; i < s.count; ++i) values.push_back(getVal<T>(s.payload, pos));
  if (pos != s.payload.size()) throw std::runtime_error("csr payload size mismatch");
  for (size_t r = 0; r < (size_t)s.rows; ++r) {
    for (std::uint64_t k = rowPtr[r]; k < rowPtr[r + 1]; ++k) {
      m.trips.push_back({r, (size_t)colIdx[(size_t)k], values[(size_t)k]});
    }
  }
  return m;
}

// ---------------------------------------------------------------------------
// staleness verification (shared by the snapshot_verify CLI and the tests)
// ---------------------------------------------------------------------------
struct VerifyResult {
  bool current = false;
  std::string reasons;  // comma-separated; empty when current
};

inline void addReason(VerifyResult& v, const std::string& r) {
  if (!v.reasons.empty()) v.reasons += ",";
  v.reasons += r;
}

inline VerifyResult verifySnapshot(const std::string& snpPath,
                                   const std::string& binaryOverride = "") {
  VerifyResult v;
  Snapshot s = loadSnapshot(snpPath);              // throws -> integrity error
  std::string manifest = loadManifestJson(snpPath);  // throws -> error

  std::string mContent = jsonField(manifest, "content_hash");
  if (mContent != hexU64(s.contentHash)) addReason(v, "manifest_content_hash_mismatch");

  std::string srcFiles = jsonField(manifest, "orig_src_files");
  std::string mSrc = jsonField(manifest, "orig_src_hash");
  if (srcFiles.empty()) {
    addReason(v, "orig_src_files_missing");
  } else {
    std::vector<std::string> files;
    std::stringstream ss(srcFiles);
    std::string item;
    while (std::getline(ss, item, ';')) {
      if (!item.empty()) files.push_back(item);
    }
    try {
      if (hexU64(filesFnv64(files)) != mSrc) addReason(v, "src_hash_mismatch");
    } catch (const std::exception&) {
      addReason(v, "orig_src_file_unreadable");
    }
  }

  std::string binPath = !binaryOverride.empty()
                            ? binaryOverride
                            : jsonField(manifest, "binary_path");
  std::string mBin = jsonField(manifest, "binary_hash");
  if (binPath.empty()) {
    addReason(v, "binary_path_missing");
  } else {
    try {
      if (hexU64(fileFnv64(binPath)) != mBin) addReason(v, "bin_hash_mismatch");
    } catch (const std::exception&) {
      addReason(v, "binary_missing");
    }
  }

  v.current = v.reasons.empty();
  return v;
}

}  // namespace snap
#endif  // UDL_SNAPSHOT_HPP
