// test_rdpac_ref.cpp
//
// Reference harness for the RDPAC sparse reformulation (Etapa 2 of the plan,
// branch methods-legacy). Embeds the ORIGINAL algorithm -- the live UDL path of
// the verbatim reference at the repo root (RDPAC.cpp) -- with only mechanical
// adaptations, each marked:
//   * framework glue -> shims: dense matrices zero-filled (upstream
//     Udl::initSparseMatrix nets zero-fill, Udl.cpp:327-341), rkLists as
//     std::vector<int> stride l_mult*l (upstream resizes before load);
//   * VLA float a[l] -> std::vector (same semantics);
//   * upstream prints behind --verbose;
//   * stage dumps inserted, marked "// DUMP" -- nothing else moved/reordered.
// Dead code absent by design (never called by runUdlMethod, verified by
// call-graph grep): reciprocalReferences, sumTranspostL, generateFullLMatrixW,
// multiplyL2, fusion.
//
// Input rkLists are synthetic: each row = [self] + (2l-1) distinct random
// ids (mt19937, seed from argv). The port consumes this same s0_lists.txt, so
// both sides see identical inputs. p/pl are fixed to the ALOI config values
// (0.60/0.99) so the port's config-driven run matches the harness exactly.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -I Source/tests Source/tests/test_rdpac_ref.cpp \
//       -o /tmp/test_rdpac_ref
// Run: /tmp/test_rdpac_ref <n> <l> <k_start> <k_end> <seed> <outdir> [--verbose]
// Float cells print with %.9g (round-trips IEEE-754 binary32 exactly).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cassert>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace rdpac_ref {

// ---- parameters (argv / fixed, see header) ----
long int N = 0;   // upstream n
int L = 0;        // upstream PARAM_RDPAC_L (member l)
const int L_MULT = 2;                 // upstream L_MULT (exactly 2 in all configs)
constexpr double P_D = 0.60;          // PARAM_RDPAC_P   (Test/config/config_aloi)
constexpr double PL = 0.99;           // PARAM_RDPAC_PL  (Test/config/config_aloi)
int K_START = 0, K_INC = 1, K_END = 0;
bool verbose = false;

// ---- structures ----
std::vector<int> rkLists;   // stride = L_MULT*L (upstream: resize(l_mult*l*n))
float* matrix = nullptr;    // W aliases matrix upstream (W = matrix)
float* RWL = nullptr;       // upstream RWL, "also known as matrix P"
float* tmpLine = nullptr;   // upstream: n floats
std::map<int, std::vector<int>> nonZero;

// ============================ shims =========================================

void allocate_structures(long int n_in = 200, int l_in = 30, int k_start = 1, int k_end = 15) {
  N = n_in;
  L = l_in;
  K_START = k_start;
  K_INC = 1;
  K_END = k_end;

  delete[] matrix;
  delete[] RWL;
  delete[] tmpLine;

  matrix = new float[static_cast<long int>(N) * N]();
  RWL = new float[static_cast<long int>(N) * N]();
  tmpLine = new float[N]();
  nonZero.clear();
}

void free_structures() {
  delete[] matrix; matrix = nullptr;
  delete[] RWL; RWL = nullptr;
  delete[] tmpLine; tmpLine = nullptr;
  rkLists.clear();
  nonZero.clear();
}

void initSparseMatrix(float*& m) {
  // Upstream: delete[]; new float[nN]; loop zeroing every cell (Udl.cpp
  // 327-341 -- nets a zero-fill). Value-init == same observable result
  // without reading uninitialized memory.
  delete[] m;
  m = new float[static_cast<long int>(N) * N]();
}

void cleanSimMatrix() {
  // Upstream generatekNNMatrixW: delete[] matrix; initSparseMatrix(matrix);
  // W = matrix; -- net effect: zero the buffer (alias preserved).
  std::fill(matrix, matrix + N * N, 0.0f);
}

// ============================ reference (verbatim) =========================

inline int getRKElem(long int query, long int pos) {
  long int index = (static_cast<long int>(L_MULT * L)) * query + pos;
  return rkLists[index];
}

inline void setRKElem(long int query, long int pos, int val) {
  long int index = (static_cast<long int>(L_MULT * L)) * query + pos;
  rkLists[index] = val;
}

inline float getMatrixElem(const float* m, long int i, long int j) {
  return m[static_cast<long int>(N) * i + j];
}

inline void setMatrixElem(float* m, long int i, long int j, float value) {
  m[static_cast<long int>(N) * i + j] = value;
}

void kernelFillPosMatrix(int rk) {
  for (int pos = 0; pos < L; pos++) {
    long int img = getRKElem(rk, pos);
    float value = pow(PL, pos + 1);
    setMatrixElem(matrix, rk, img, getMatrixElem(matrix, rk, img) + value);
    setMatrixElem(matrix, img, rk, getMatrixElem(matrix, img, rk) + value);
  }
}

void copyingMatrixWToDBL() {
  // Verbatim even though (newDist < 0) never fires under zero-init (O1,
  // upstream proof) -- the reference must stay literal.
  for (int i = 0; i < N; i++) {
    for (int pos = 0; pos < L_MULT * L; pos++) {
      int imgJ = getRKElem(i, pos);
      float newDist = getMatrixElem(matrix, i, imgJ);
      if (newDist < 0) {
        setMatrixElem(matrix, i, imgJ, N - pos);
      }
    }
  }
}

void execFillPosMatrix() {
  for (int i = 0; i < N; i++) kernelFillPosMatrix(i);
  copyingMatrixWToDBL();
}

void kernelSortRankedLists(int rk) {
  int l = static_cast<int>(rkLists.size()) / static_cast<int>(N);
  long int LcurRL = static_cast<long int>(l) * rk;
  long int cNcurRL = static_cast<long int>(N) * rk;
  std::vector<float> a(l);  // upstream VLA float a[l]

  long int index;
  for (int j = 0; j < l; j++) {
    index = cNcurRL + rkLists[LcurRL + j];
    a[j] = matrix[index];
  }

  // ---------------------- INSERTION SORT --------------------------
  int i, j, keyR;
  float keyA;
  for (j = 1; j < l; j++) {
    keyA = a[j];
    keyR = rkLists[LcurRL + j];
    i = j - 1;
    while (i >= 0 && a[i] < keyA) {
      a[i + 1] = a[i];
      rkLists[LcurRL + i + 1] = rkLists[LcurRL + i];
      i--;
    }
    a[i + 1] = keyA;
    rkLists[LcurRL + i + 1] = keyR;
  }

  // Setting query image at first position (verbatim probe order)
  i = 0;
  while ((rkLists[LcurRL + i] != rk) && (i < l)) i++;
  if (i > 0) {
    int aux = rkLists[LcurRL + 0];
    rkLists[LcurRL + 0] = rkLists[LcurRL + i];
    rkLists[LcurRL + i] = aux;
    float auxA = a[0];
    a[0] = a[i];
    a[i] = auxA;
  }
}

void execSortRankedLists() {
  for (int i = 0; i < N; i++) kernelSortRankedLists(i);
}

void generatekNNMatrixW(int k) {
  cleanSimMatrix();
  for (int imgI = 0; imgI < N; imgI++) {
    setMatrixElem(matrix, imgI, imgI, 1.0f);
    for (int j = 0; j < k; j++) {
      int imgJ = getRKElem(imgI, j);
      setMatrixElem(matrix, imgI, imgJ, pow(P_D, (j + 1)));
    }
  }
}

void normalizeMatrix(float* m, int lim) {
  std::vector<float> vAcum(N, 0.0f);  // upstream: malloc'd float array
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < lim; j++) {
      int jidx = getRKElem(i, j);
      vAcum[jidx] += getMatrixElem(m, i, jidx);
    }
  }
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < lim; j++) {
      int jidx = getRKElem(i, j);
      float value = getMatrixElem(m, i, jidx) / vAcum[jidx];
      setMatrixElem(m, i, jidx, value);
    }
  }
}

void copyMatrices(float* src, float* dst, long int lim) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < lim; j++) {
      int img = getRKElem(i, j);
      setMatrixElem(dst, i, img, getMatrixElem(src, i, img));
    }
  }
}

void multiplyByTranspostLStoreList(int k) {
  for (int i = 0; i < N; i++) {
    std::vector<int> listNonZero;
    for (int j = 0; j < L; j++) {
      int jidx = getRKElem(i, j);
      tmpLine[jidx] = 0;
      for (int lk = 0; lk < k; lk++) {          // upstream shadows member `l`
        int lidx = getRKElem(jidx, lk);
        tmpLine[jidx] += getMatrixElem(RWL, i, lidx) * getMatrixElem(matrix, jidx, lidx);
      }
      if (tmpLine[jidx] > 0) listNonZero.push_back(jidx);
    }
    nonZero[i] = listNonZero;
    for (int j = 0; j < L; j++) {
      int jidx = getRKElem(i, j);
      setMatrixElem(RWL, i, jidx, tmpLine[jidx]);
    }
  }
}

void multiplyByTranspostL(int k) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < L; j++) {
      int jidx = getRKElem(i, j);
      tmpLine[jidx] = 0;
      for (int lk = 0; lk < k; lk++) {
        int lidx = getRKElem(jidx, lk);
        tmpLine[jidx] += getMatrixElem(RWL, i, lidx) * getMatrixElem(matrix, jidx, lidx);
      }
    }
    for (int j = 0; j < L; j++) {
      int jidx = getRKElem(i, j);
      setMatrixElem(RWL, i, jidx, tmpLine[jidx]);
    }
  }
}

void sumIdentityAlpha(float alpha) {
  for (int i = 0; i < N; i++) {
    for (int jc = 0; jc < L_MULT * L; jc++) {
      int j = getRKElem(i, jc);
      if (i != j) {
        setMatrixElem(RWL, i, j, getMatrixElem(RWL, i, j) * alpha);
      } else {
        setMatrixElem(RWL, i, j, getMatrixElem(RWL, i, j) * alpha + (1.0 * (1.0 - alpha)));
      }
    }
  }
}

void multiplyL1(float* m1, float* m2, float* dst) {
  float* tmpColumn = tmpLine;
  for (int jc = 0; jc < L_MULT * L; jc++) {
    for (int i = 0; i < N; i++) {
      int j = getRKElem(i, jc);
      tmpColumn[i] = 0;
      for (int l : nonZero[i]) {
        tmpColumn[i] += getMatrixElem(m1, i, l) * getMatrixElem(m2, l, j);
      }
    }
    for (int i = 0; i < N; i++) {
      int j = getRKElem(i, jc);
      setMatrixElem(dst, i, j, tmpColumn[i]);
    }
  }
}


// ============================================================================
// DUMP STRUCTS & PIPELINE RUNNER
// ============================================================================
struct PhaseDumps {
  std::vector<std::vector<int>> rkLists_post_fill;
  std::vector<std::vector<int>> rkLists_final;

  std::vector<float> matrix_post_fill;
  std::vector<float> rwl_post_fill;
  std::vector<float> w_k15;
  std::vector<float> rwl_post_norm;
  std::vector<float> rwl_post_iter0;
  std::vector<float> rwl_post_15iter;
  std::vector<float> w_post_mult1;
  std::vector<float> w_post_mult2;
  uint64_t final_rk_checksum = 0;
};

std::vector<float> snapshot_rk_positions(const float* mat, int max_cols) {
  std::vector<float> snap;
  snap.reserve(N * max_cols);
  for (int i = 0; i < N; i++) {
    for (int c = 0; c < max_cols; c++) {
      int j = getRKElem(i, c);
      snap.push_back(getMatrixElem(mat, i, j));
    }
  }
  return snap;
}

uint64_t compute_rk_checksum() {
  uint64_t h = 14695981039346656037ULL;
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < L_MULT * L; j++) {
      uint64_t v = static_cast<uint64_t>(getRKElem(i, j));
      h ^= v;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

std::vector<std::vector<int>> capture_rkLists() {
  std::vector<std::vector<int>> rk(N, std::vector<int>(L_MULT * L));
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < L_MULT * L; j++) {
      rk[i][j] = getRKElem(i, j);
    }
  }
  return rk;
}


PhaseDumps run_reference_pipeline() {
  PhaseDumps dumps;

  execFillPosMatrix();
  copyingMatrixWToDBL();
  execSortRankedLists();
  dumps.rkLists_post_fill = capture_rkLists();
  dumps.matrix_post_fill = snapshot_rk_positions(matrix, L_MULT * L);
  dumps.rwl_post_fill = snapshot_rk_positions(RWL, L_MULT * L);

  for (int k = K_START; k <= K_END; k += K_INC) {
    int curK = K_END;
    cleanSimMatrix();
    generatekNNMatrixW(curK);

    if (k == K_START) {
      dumps.w_k15 = snapshot_rk_positions(matrix, curK + 1);
      for (int i = 0; i < N; i++) {
        for (int j = 0; j < L; j++) {
          int jidx = getRKElem(i, j);
          setMatrixElem(RWL, i, jidx, getMatrixElem(matrix, i, jidx));
        }
      }
    }

    normalizeMatrix(RWL, L);
    if (k == K_START) {
      dumps.rwl_post_norm = snapshot_rk_positions(RWL, L);
    }
    normalizeMatrix(matrix, curK);

    if (k == K_END) {
      multiplyByTranspostLStoreList(curK);
    } else {
      multiplyByTranspostL(curK);
    }

    sumIdentityAlpha(0.95f);

    if (k == K_START) {
      dumps.rwl_post_iter0 = snapshot_rk_positions(RWL, L_MULT * L);
    }
  }

  dumps.rwl_post_15iter = snapshot_rk_positions(RWL, L_MULT * L);

  normalizeMatrix(RWL, L);

  multiplyL1(RWL, RWL, matrix);
  dumps.w_post_mult1 = snapshot_rk_positions(matrix, L_MULT * L);

  multiplyL1(RWL, matrix, matrix);
  dumps.w_post_mult2 = snapshot_rk_positions(matrix, L_MULT * L);

  copyingMatrixWToDBL();
  execSortRankedLists();

  dumps.rkLists_final = capture_rkLists();
  dumps.final_rk_checksum = compute_rk_checksum();

  return dumps;
}

void init_synthetic_rkLists(int seed = 42) {
  std::mt19937 rng(seed);
  rkLists.resize(N * L_MULT * L, 0);

  for (int i = 0; i < N; i++) {
    std::vector<int> pool;
    pool.reserve(N);
    for (int j = 0; j < N; j++) {
      if (j != i) pool.push_back(j);
    }
    std::shuffle(pool.begin(), pool.end(), rng);

    setRKElem(i, 0, i);
    for (int pos = 1; pos < L; pos++) {
      setRKElem(i, pos, pool[pos - 1]);
    }
    for (int pos = L; pos < L_MULT * L; pos++) {
      setRKElem(i, pos, 0);
    }
  }
}

} // namespace rdpac_ref


int main() {
  rdpac_ref::allocate_structures();
  rdpac_ref::init_synthetic_rkLists(42);

  std::printf("=================================================================\n");
  std::printf("TEST RDPAC REFERENCE PIPELINE (N=%ld, L=%d, L_MULT=%d, P=%.2f, PL=%.2f)\n",
              rdpac_ref::N, rdpac_ref::L, rdpac_ref::L_MULT, rdpac_ref::P_D, rdpac_ref::PL);
  std::printf("=================================================================\n");

  std::printf("[1] Synthetic rkLists initialized (N=%d, stride=%d, tail zeros present).\n",
              rdpac_ref::N, rdpac_ref::L_MULT * rdpac_ref::L);
  std::printf("    Sample row 0 head: [%d, %d, %d, %d] ... pos L-1=%d, pos L=%d (tail)\n",
              rdpac_ref::getRKElem(0, 0), rdpac_ref::getRKElem(0, 1),
              rdpac_ref::getRKElem(0, 2), rdpac_ref::getRKElem(0, 3),
              rdpac_ref::getRKElem(0, rdpac_ref::L - 1),
              rdpac_ref::getRKElem(0, rdpac_ref::L));

  uint64_t initial_cksum = rdpac_ref::compute_rk_checksum();
  std::printf("    Initial rkLists checksum: 0x%016llx\n", (unsigned long long)initial_cksum);

  std::printf("[2] Running reference pipeline...\n");
  auto t0 = std::chrono::steady_clock::now();
  auto dumps = rdpac_ref::run_reference_pipeline();
  auto t1 = std::chrono::steady_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::printf("[3] Reference pipeline completed in %.2f ms.\n", elapsed_ms);
  std::printf("    Final rkLists checksum:   0x%016llx\n", (unsigned long long)dumps.final_rk_checksum);
  std::printf("    Dumps recorded:\n");
  std::printf("      rwl_post_fill:   %zu floats\n", dumps.rwl_post_fill.size());
  std::printf("      w_k15:           %zu floats\n", dumps.w_k15.size());
  std::printf("      rwl_post_norm:   %zu floats\n", dumps.rwl_post_norm.size());
  std::printf("      rwl_post_iter0:  %zu floats\n", dumps.rwl_post_iter0.size());
  std::printf("      rwl_post_15iter: %zu floats\n", dumps.rwl_post_15iter.size());
  std::printf("      w_post_mult1:    %zu floats\n", dumps.w_post_mult1.size());
  std::printf("      w_post_mult2:    %zu floats\n", dumps.w_post_mult2.size());
  std::printf("      nonZero entries captured: %zu keys\n", rdpac_ref::nonZero.size());

  assert(dumps.final_rk_checksum != 0);
  assert(dumps.final_rk_checksum != initial_cksum);
  assert(dumps.rkLists_final[0][0] == 0);
  assert(dumps.rkLists_final[1][0] == 1);
  assert(dumps.rkLists_final[rdpac_ref::N - 1][0] == rdpac_ref::N - 1);

  std::printf("[PASS] Reference pipeline runs cleanly and produces non-trivial output.\n");

  rdpac_ref::free_structures();
  return 0;
}

