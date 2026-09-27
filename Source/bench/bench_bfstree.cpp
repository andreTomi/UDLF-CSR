// bench_bfstree.cpp -- Etapa 2 do port BFSTree: campanha A / A2 / E.
//
// Um processo por variante (ru_maxrss limpo). Modos:
//   ref  : A denso verbatim; escreve rkLists finais em <refpath>
//   A    : A denso verbatim; compara vs <refpath> (bit + P@k)
//   A2   : F5 bloqueado exato (indice invertido) + saidas vs <refpath>
//   E    : SpGEMM (3 produtos + merge, multiply_and_prune) vs <refpath>
//   Eattr: atribuicao celula-a-celula W_E vs W_A (3 fontes de erro)
// Uso: bench_bfstree <mode> <N> <L> <k> <seed> <B> <limit> <refpath>
// Saida: linhas key=value (T_*=ms, RSS_KB, DENSITY, CK_W, CK_FINAL, P*@,
//        BITFINAL, ATTR_*).
//
// Desenho incorporando as 4 condicoes da Etapa 1:
//  (1) tres fontes de erro de E vs A separadas: (i) float -- gates herdados
//      max_rel=5.0e-6 / max_abs=4.2e-4 (T4a-full); (ii) filtro |v|>1e-9
//      (independe do limit); (iii) poda por limit. Isolamento: E com
//      limit>=N (nao poda; teto uint32 respeitado) vs A => (i)+(ii);
//      decremento do limit => (iii). Eattr quantifica as 3 por contagem.
//  (2) pin: incondicional dado self na linha (garantia de producao
//      documentada em test_bfstree_hyp.cpp); gerador force self na pos 0.
//  (3) gates: A2 comparado contra a SAIDA FINAL de A (rkLists pos-F9),
//      bit-identico exigido.
//  (4) tolerancias herdados: nenhum gate mais folgado sem justificativa.
//  Constraints: size_type=uint32 (nnz<4.29e9); densidade MEDIDA por ponto;
//  pontos incluem regime k^4/N~2.2 (ALOI) e saturado (k=20).
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
//       Source/bench/bench_bfstree.cpp -o /tmp/bench_bfstree

#include "Core/hpp/SparseMatrix.hpp"

#include <omp.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static long rss_kb() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;  // KB on Linux
}
// Peak-RSS checkpoint. ru_maxrss is a HIGH-WATER MARK (never decreases), so a
// trace of checkpoints is non-decreasing and each JUMP attributes the peak to
// the structure that was live when it happened. This is how the scale model
// gets measured instead of being inferred from a single end-of-run RSS.
static bool memtrace_on() {
    static const bool on = std::getenv("BFSTREE_MEMTRACE") != nullptr;
    return on;
}
static void memprobe(const char* tag) {
    if (!memtrace_on()) return;
    std::fflush(stdout);
    std::printf("MEMPROBE=%s RSS_KB=%ld\n", tag, rss_kb());
    std::fflush(stdout);
}
static uint64_t fnv_bytes(const void* p, size_t nbytes) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < nbytes; ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
// ===========================================================================
// LEGACY REFERENCE (verbatim from repo-root BFSTree.cpp -- F1..F9)
// ===========================================================================
namespace leg {

long int n = 0;
int l = 0;        // rkLists stride
int topK = 0;     // = k
std::vector<int> rkLists;                  // n*l
std::vector<float> matrix;                 // n*n dense (A)
std::vector<std::map<int, float>> imgCor;  // n maps

inline int getRKElem(long int query, long int pos) {
    return rkLists[(long int)l * query + pos];
}
inline float getMatrixElem(std::vector<float>& m, long int i, long int j) {
    return m[(long int)n * i + j];
}
inline void setMatrixElem(std::vector<float>& m, long int i, long int j,
                          float v) {
    m[(long int)n * i + j] = v;
}
inline float minZero(float x, float y) {
    if (x == 0) return y;
    return std::min(x, y);
}
inline void incSimW(int img1, int img2, float newSim) {
    setMatrixElem(matrix, img1, img2,
                  getMatrixElem(matrix, img1, img2) + newSim);
}
void zero_matrix() { std::fill(matrix.begin(), matrix.end(), 0.0f); }

// --- sorting + pin (verbatim; see test_bfstree_hyp.cpp for the pin guarantee)
void kernelSortRankedLists(int rk) {
    int l_ = (int)(rkLists.size() / n);
    long int LcurRL = ((long int)l_) * rk;
    long int cNcurRL = ((long int)n) * rk;
    float a[l_];
    long int index;
    for (int j = 0; j < l_; j++) {
        index = cNcurRL + rkLists[LcurRL + j];
        a[j] = matrix[index];
    }
    int i, j, keyR;
    float keyA;
    for (j = 1; j < l_; j++) {
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
    i = 0;
    while ((rkLists[LcurRL + i] != rk) && (i < l_)) {
        i++;
    }
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
    for (int i = 0; i < n; i++) kernelSortRankedLists(i);
}

// --- computeRankNormalization (verbatim) ------------------------------------
void computeRankNormalization(int type) {
    switch (type) {
        case 1:
            for (int qImg = 0; qImg < n; qImg++) {
                for (int pos = 0; pos < l; pos++) {
                    int img = getRKElem(qImg, pos);
                    setMatrixElem(matrix, qImg, img,
                                  getMatrixElem(matrix, qImg, img) + (l - pos));
                    setMatrixElem(matrix, img, qImg,
                                  getMatrixElem(matrix, img, qImg) + (l - pos));
                }
            }
            break;
        case 2:
            for (int qImg = 0; qImg < n; qImg++) {
                for (int pos = 0; pos < l; pos++) {
                    int img = getRKElem(qImg, pos);
                    setMatrixElem(matrix, qImg, img,
                                  minZero(getMatrixElem(matrix, qImg, img),
                                          (l - pos)));
                    setMatrixElem(matrix, img, qImg,
                                  minZero(getMatrixElem(matrix, img, qImg),
                                          (l - pos)));
                }
            }
            break;
    }
    execSortRankedLists();
}
// --- rbo + computeRankCorrelation (verbatim; config default RBO) ------------
float rbo(int i1, int i2) {
    float p = 0.7;
    float inter = 0;
    float result = 0;
    int k = 1;
    while (k <= topK) {
        inter = 0;
        for (int i = 0; i < k; i++) {
            for (int j = 0; j < k; j++) {
                if (getRKElem(i1, i) == getRKElem(i2, j)) {
                    inter += 1;
                }
            }
        }
        result += pow(p, k - 1) * (inter / k);
        k++;
    }
    result = (1 - p) * result;
    return result;
}
void computeRankCorrelation() {
    imgCor.clear();
    imgCor.resize(n);
    for (int qImg = 0; qImg < n; qImg++) {
        imgCor[qImg][qImg] = 1.0;
        for (int pos = 0; pos < topK; pos++) {
            int img = getRKElem(qImg, pos);
            imgCor[qImg][img] = rbo(qImg, img);
        }
    }
}

// --- F5: computeIncTree + acumBFSTree (verbatim, incType=2) -----------------
void computeIncTree(int qImg) {
    std::vector<int> l1;
    std::vector<int> l2;
    std::vector<double> l2d;

    l1.push_back(qImg);

    for (int i = 1; i < topK; i++) {
        int imgI = getRKElem(qImg, i);
        l1.push_back(imgI);
        float ircor = imgCor[qImg][imgI];
        for (int j = 1; j < topK; j++) {
            int imgJ = getRKElem(imgI, j);
            l2.push_back(imgJ);
            float jcor = ircor * imgCor[imgI][imgJ];
            l2d.push_back(jcor);
        }
    }
    // `if (incType != 2)` early increments are dead code under incType=2.
    for (int img1a : l1) {
        for (int img1b : l1) {
            incSimW(img1a, img1b, imgCor[qImg][img1a] * imgCor[qImg][img1b]);
        }
    }
    for (size_t i = 0; i < l2.size(); i++) {
        for (size_t j = 0; j < l2.size(); j++) {
            incSimW(l2[i], l2[j], l2d[i] * l2d[j]);
        }
    }
    for (size_t i = 0; i < l1.size(); i++) {
        for (size_t j = 0; j < l2.size(); j++) {
            incSimW(l1[i], l2[j], imgCor[qImg][l1[i]] * l2d[j]);
        }
    }
}
void acumBFSTree() {
    for (int qimg = 0; qimg < n; qimg++) computeIncTree(qimg);
}

// --- F6: normalizeWProb (verbatim; column sums in DOUBLE, +1.0 smoothing) ---
void normalizeWProb() {
    for (int i = 0; i < n; i++) {
        double acum = 1.0;
        for (int j = 0; j < n; j++) {
            acum += getMatrixElem(matrix, j, i);
        }
        for (int j = 0; j < n; j++) {
            setMatrixElem(matrix, j, i,
                          getMatrixElem(matrix, j, i) / acum);
        }
    }
}

// --- F7: computeWDiffusion (verbatim; fresh n*n rc, then dense copy) --------
void computeWDiffusion() {
    std::vector<float> rc((size_t)n * (size_t)n, 0.0f);

    for (int imgI = 0; imgI < n; imgI++) {
        for (int posJ = 0; posJ < l; posJ++) {
            int imgJ = getRKElem(imgI, posJ);
            for (int posM = 0; posM < l; posM++) {
                int imgM = getRKElem(imgI, posM);
                float value = getMatrixElem(matrix, imgI, imgM) *
                              getMatrixElem(matrix, imgJ, imgM);
                setMatrixElem(rc, imgI, imgJ,
                              getMatrixElem(rc, imgI, imgJ) + value);
            }
        }
    }
    matrix = rc;  // legacy: dense copy rc->matrix (same values, O(N^2) copy),
                  // then rc freed at scope end
}

}  // namespace leg

// ===========================================================================
// Pipeline A (dense verbatim F1..F9) + instrumentation
// ===========================================================================
struct PhaseTimes {
    double f1f2 = 0, f3 = 0, f5 = 0, f6 = 0, f7 = 0, f8 = 0, f9 = 0;
    double total = 0;
};

static void gen_rklists(int seed, int N, int L) {
    leg::n = N;
    leg::l = L;
    leg::rkLists.assign((size_t)N * L, 0);
    leg::matrix.assign((size_t)N * N, 0.0f);
    std::mt19937 rng(seed);
    std::vector<int> perm(N);
    for (int i = 0; i < N; ++i) perm[i] = i;
    for (int i = 0; i < N; ++i) {
        std::shuffle(perm.begin(), perm.end(), rng);
        // Pin contract: self at position 0 (production convention; the
        // pin-guarantee is documented in test_bfstree_hyp.cpp).
        auto it = std::find(perm.begin(), perm.end(), i);
        std::swap(perm[0], *it);
        for (int j = 0; j < L; ++j)
            leg::rkLists[(size_t)i * L + j] = perm[j];
    }
}

// ---------------------------------------------------------------------------
// Real-structure loader.
//
// Why: every rho/density number in Etapas 2-4 came from gen_rklists(), i.e.
// from RANDOM rank lists. Whether the W density of the production config
// (L=1400, k=20) holds on ALOI's real rank correlation is a separate fact, and
// the model must not be allowed to silently inherit the synthetic structure.
//
// Sub-corpus semantics: real rkLists.txt rows are rank lists over the whole
// corpus (72000 ids, 7200 tokens/row measured). For a run with N < 72000 we
// keep only ids < N (dropping the rest, preserving order) and take the first L
// survivors -- exactly the rank lists a real BFSTree run on that N-subset
// would see. With 7200 tokens/row and N=24000 of 72000 kept, ~2400 survivors
// per row >= L=1400; the loader verifies it per row and reports the census.
// No pinning and no shuffling: whatever order the real data has is used.
// ---------------------------------------------------------------------------
struct RealLoadStats {
    long rows_read = 0;      // rows consumed from the file
    long rows_short = 0;     // rows with < L survivors after subsetting
    long self_missing = 0;   // rows without the query itself (contract break)
    long self_at0 = 0;       // rows with self at position 0 (production form)
    long min_survivors = 0;  // min survivors over rows
    double mean_survivors = 0;
};

static bool load_real_rk(const std::string& path, int N, int L, bool want_matrix,
                         RealLoadStats* st) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    leg::n = N;
    leg::l = L;
    leg::rkLists.assign((size_t)N * L, 0);
    if (want_matrix)
        leg::matrix.assign((size_t)N * (size_t)N, 0.0f);  // F1/F2 scratch, as in A
    char* line = nullptr;
    size_t cap = 0;
    long kept_total = 0, min_surv = (long)1 << 30;
    for (int i = 0; i < N; ++i) {
        ssize_t len = getline(&line, &cap, f);
        if (len < 0) { std::fclose(f); free(line); return false; }
        st->rows_read++;
        const char* p = line;
        const char* end = line + len;
        int* dst = &leg::rkLists[(size_t)i * L];
        long kept = 0, surv = 0;
        while (p < end && kept < L) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                               *p == '\r')) ++p;
            if (p >= end || *p < '0' || *p > '9') break;
            long id = std::strtol(p, (char**)&p, 10);
            if (id < N) {                 // subset-keeping, order preserved
                ++surv;
                dst[kept++] = (int)id;
            }
        }
        // drain the rest of the row to count total survivors in the subset
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                               *p == '\r')) ++p;
            if (p >= end || *p < '0' || *p > '9') break;
            long id = std::strtol(p, (char**)&p, 10);
            if (id < N) ++surv;
        }
        if (kept < L) st->rows_short++;
        if (surv < min_surv) min_surv = surv;
        kept_total += surv;
        bool has_self = false;
        for (long q = 0; q < kept; ++q) if (dst[q] == i) { has_self = true; break; }
        if (!has_self) st->self_missing++;
        else if (dst[0] == i) st->self_at0++;
    }
    free(line);
    std::fclose(f);
    st->min_survivors = min_surv;
    st->mean_survivors = (double)kept_total / (double)std::max(1L, st->rows_read);
    return true;
}

// F1..F4 (correlation) shared by every variant.
static void pipeline_front() {
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    leg::computeRankCorrelation();
    leg::zero_matrix();  // F4: the normalization matrix is scratch; W resets
}

static long count_density(const std::vector<float>& w, long* nonzero) {
    long cells = 0;
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] != 0.0f) ++cells;
    if (nonzero) *nonzero = cells;
    return (long)w.size();
}
static uint64_t fnv_rk() {
    return fnv_bytes(leg::rkLists.data(), leg::rkLists.size() * sizeof(int));
}
static uint64_t fnv_w(const std::vector<float>& w) {
    return fnv_bytes(w.data(), w.size() * sizeof(float));
}

// Run dense A: F1..F9 with phase times; density measured on the F5 output.
// ck_w_out (optional) receives the FNV of the raw F5 output (pre-F6) so
// variants can compare W-level, not just final-rk-level.
static uint64_t run_A(PhaseTimes* t, uint64_t* ck_w_out, long* nnz_out) {
    const auto t_all = Clock::now();
    auto t0 = Clock::now();
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    t->f1f2 = ms_since(t0);

    t0 = Clock::now();
    leg::computeRankCorrelation();
    t->f3 = ms_since(t0);

    t0 = Clock::now();
    leg::zero_matrix();  // F4
    leg::acumBFSTree();  // F5
    t->f5 = ms_since(t0);
    long nnz = 0;
    count_density(leg::matrix, &nnz);
    if (nnz_out) *nnz_out = nnz;
    if (ck_w_out) *ck_w_out = fnv_w(leg::matrix);
    memprobe("A_afterF5");           // W dense live; rc not yet allocated

    t0 = Clock::now();
    leg::normalizeWProb();  // F6
    t->f6 = ms_since(t0);

    t0 = Clock::now();
    leg::computeWDiffusion();  // F7  <-- allocates the SECOND dense n*n (rc)
    t->f7 = ms_since(t0);
    memprobe("A_afterF7");           // peak of the legacy two-dense-array form

    t0 = Clock::now();
    leg::normalizeWProb();  // F8
    t->f8 = ms_since(t0);

    t0 = Clock::now();
    leg::execSortRankedLists();  // F9
    t->f9 = ms_since(t0);
    memprobe("A_end");

    t->total = ms_since(t_all);
    return fnv_rk();
}

// ---- reference file IO (raw int rkLists, N*L) ------------------------------
static bool write_ref(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t got = std::fwrite(leg::rkLists.data(), sizeof(int),
                             leg::rkLists.size(), f);
    std::fclose(f);
    return got == leg::rkLists.size();
}
static bool read_ref(const std::string& path, std::vector<int>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(bytes / (long)sizeof(int));
    size_t got = std::fread(out->data(), sizeof(int), out->size(), f);
    std::fclose(f);
    return got == out->size();
}

// ---- final comparison: bit-exact + P@k (top-k overlap vs reference) --------
struct CmpOut {
    bool bit = false;
    double p1 = 0, p5 = 0, p10 = 0, p20 = 0;
    long total_cells = 0, equal_cells = 0;
};
static CmpOut compare_final(const std::vector<int>& ref) {
    CmpOut c;
    const size_t N = (size_t)leg::n, L = (size_t)leg::l;
    if (ref.size() != N * L) return c;
    c.bit = std::memcmp(ref.data(), leg::rkLists.data(),
                        N * L * sizeof(int)) == 0;
    const int ks[4] = {1, 5, 10, 20};
    double acc[4] = {0, 0, 0, 0};
    for (size_t q = 0; q < N; ++q) {
        c.total_cells += (long)L;
        for (size_t p = 0; p < L; ++p)
            if (ref[q * L + p] == leg::rkLists[q * L + p]) ++c.equal_cells;
        for (int ki = 0; ki < 4; ++ki) {
            int kk = std::min<int>(ks[ki], (int)L);
            int hits = 0;
            for (int a = 0; a < kk; ++a) {
                int v = leg::rkLists[q * L + a];
                for (int b = 0; b < kk; ++b)
                    if (ref[q * L + b] == v) { ++hits; break; }
            }
            acc[ki] += (double)hits / kk;
        }
    }
    c.p1 = acc[0] / N; c.p5 = acc[1] / N; c.p10 = acc[2] / N;
    c.p20 = acc[3] / N;
    return c;
}
// ===========================================================================
// main -- mode dispatch (E / A2 / Eattr added below)
// ===========================================================================
static uint64_t run_E(size_t limit, PhaseTimes* t, long* nnz_w_out);
static uint64_t run_A2(long B, PhaseTimes* t, long* nnz_w_out,
                       uint64_t* ck_w_out);
static uint64_t run_A3(bool alg, int kern, long B, PhaseTimes* t,
                       long* nnz_w_out, uint64_t* ck_w_out);
template <bool NODIV>
static M compute_w_diffusion_a4(long B, const std::vector<double>& ac);
static int run_f6a();
static int run_eattr(size_t limit);
static int run_touch();

static void print_common(const char* mode, int N, int L, int k, int seed,
                         long B, size_t limit, const PhaseTimes& t,
                         uint64_t ck_w, long nnz_w, uint64_t ck_final) {
    long n2 = (long)N * (long)N;
    std::printf("MODE=%s N=%d L=%d k=%d seed=%d B=%ld limit=%zu\n", mode, N, L,
                k, seed, B, limit);
    std::printf("DENSITY=%.6f NNZ_W=%ld N2=%ld\n",
                n2 > 0 ? (double)nnz_w / (double)n2 : 0.0, nnz_w, n2);
    std::printf("T_F1F2_MS=%.2f T_F3_MS=%.2f T_F5_MS=%.2f T_F6_MS=%.2f "
                "T_F7_MS=%.2f T_F8_MS=%.2f T_F9_MS=%.2f T_TOTAL_MS=%.2f\n",
                t.f1f2, t.f3, t.f5, t.f6, t.f7, t.f8, t.f9, t.total);
    std::printf("RSS_KB=%ld\n", rss_kb());
    std::printf("CK_W=0x%016llx CK_FINAL=0x%016llx\n",
                (unsigned long long)ck_w, (unsigned long long)ck_final);
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::fprintf(stderr,
            "usage: bench_bfstree <ref|A> <N> <L> <k> <seed> <B> <limit> "
            "[refpath]\n");
        return 2;
    }
    std::string mode = argv[1];
    int N = std::atoi(argv[2]), L = std::atoi(argv[3]), k = std::atoi(argv[4]);
    int seed = std::atoi(argv[5]);
    long B = std::atol(argv[6]);
    size_t limit = (size_t)std::strtoull(argv[7], nullptr, 10);
    std::string refpath = (argc > 8) ? argv[8] : "";
    std::string dataspec = (argc > 9) ? argv[9] : "";   // "real:<rkLists.txt>"
    if (L < k || N < L) {
        std::fprintf(stderr, "need N >= L >= k\n");
        return 2;
    }

    if (dataspec.rfind("real:", 0) == 0) {
        RealLoadStats st;
        if (!load_real_rk(dataspec.substr(5), N, L, mode != "TOUCHRAW", &st)) {
            std::fprintf(stderr, "cannot load real rank lists: %s\n",
                         dataspec.c_str());
            return 3;
        }
        std::printf("DATA=REAL path=%s N=%d L=%d rows_read=%ld rows_short=%ld "
                    "self_missing=%ld self_at0=%ld min_survivors=%ld "
                    "mean_survivors=%.1f\n",
                    dataspec.c_str(), N, L, st.rows_read, st.rows_short,
                    st.self_missing, st.self_at0, st.min_survivors,
                    st.mean_survivors);
        if (st.self_missing > 0) {
            std::fprintf(stderr,
                         "contract break: %ld rows without self (production "
                         "guarantees self in the list); refusing to continue\n",
                         st.self_missing);
            return 4;
        }
        if (st.rows_short > 0) {
            std::fprintf(stderr,
                         "contract break: %ld rows with < L survivors; "
                         "refusing to continue\n", st.rows_short);
            return 5;
        }
    } else {
        gen_rklists(seed, N, L);
        std::printf("DATA=SYNTH seed=%d\n", seed);
    }
    leg::topK = k;

    PhaseTimes t;
    uint64_t ck_w = 0, ck_final = 0;
    long nnz_w = 0;

    if (mode == "ref" || mode == "A") {
        ck_final = run_A(&t, &ck_w, &nnz_w);
        print_common(mode.c_str(), N, L, k, seed, B, limit, t, ck_w, nnz_w,
                     ck_final);
        if (mode == "ref") {
            if (refpath.empty() || !write_ref(refpath)) {
                std::fprintf(stderr, "cannot write ref %s\n", refpath.c_str());
                return 3;
            }
            std::printf("REF_WRITTEN=%s\n", refpath.c_str());
        } else if (!refpath.empty()) {
            std::vector<int> ref;
            if (read_ref(refpath, &ref)) {
                CmpOut c = compare_final(ref);
                std::printf("BITFINAL=%s POS_EQUAL=%.6f P1=%.6f P5=%.6f "
                            "P10=%.6f P20=%.6f\n",
                            c.bit ? "YES" : "NO",
                            c.total_cells ? (double)c.equal_cells /
                                            c.total_cells : 0.0,
                            c.p1, c.p5, c.p10, c.p20);
            } else {
                std::printf("BITFINAL=SKIP\n");
            }
        }
        return 0;
    }
    if (mode == "E") {
        ck_final = run_E(limit, &t, &nnz_w);
        print_common(mode.c_str(), N, L, k, seed, B, limit, t, 0, nnz_w,
                     ck_final);
        if (!refpath.empty()) {
            std::vector<int> ref;
            if (read_ref(refpath, &ref)) {
                CmpOut c = compare_final(ref);
                std::printf("BITFINAL=%s POS_EQUAL=%.6f P1=%.6f P5=%.6f "
                            "P10=%.6f P20=%.6f\n",
                            c.bit ? "YES" : "NO",
                            c.total_cells ? (double)c.equal_cells /
                                            c.total_cells : 0.0,
                            c.p1, c.p5, c.p10, c.p20);
            } else {
                std::printf("BITFINAL=SKIP\n");
            }
        }
        return 0;
    }
    if (mode == "A2") {
        ck_final = run_A2(B, &t, &nnz_w, &ck_w);
        print_common(mode.c_str(), N, L, k, seed, B, limit, t, ck_w, nnz_w,
                     ck_final);
        if (!refpath.empty()) {
            std::vector<int> ref;
            if (read_ref(refpath, &ref)) {
                CmpOut c = compare_final(ref);
                std::printf("BITFINAL=%s POS_EQUAL=%.6f P1=%.6f P5=%.6f "
                            "P10=%.6f P20=%.6f\n",
                            c.bit ? "YES" : "NO",
                            c.total_cells ? (double)c.equal_cells /
                                            c.total_cells : 0.0,
                            c.p1, c.p5, c.p10, c.p20);
            } else {
                std::printf("BITFINAL=SKIP\n");
            }
        }
        return 0;
    }
    if (mode == "Eattr") {
        leg::zero_matrix();
        leg::computeRankNormalization(1);
        leg::computeRankNormalization(2);
        leg::computeRankCorrelation();
        return run_eattr(limit);
    }
    if (mode == "F6a") return run_f6a();
    if (mode == "TOUCH" || mode == "TOUCHRAW") {
        if (mode == "TOUCH") leg::zero_matrix(), pipeline_front();
        return run_touch();
    }
    if (mode == "A3" || mode == "A3a" || mode == "A4" || mode == "A4b") {
        ck_final = run_A3(mode == "A3a",
                          mode == "A4" ? 1 : mode == "A4b" ? 2 : 0, B, &t,
                          &nnz_w, &ck_w);
        print_common(mode.c_str(), N, L, k, seed, B, limit, t, ck_w, nnz_w,
                     ck_final);
        if (!refpath.empty()) {
            std::vector<int> ref;
            if (read_ref(refpath, &ref)) {
                CmpOut c = compare_final(ref);
                std::printf("BITFINAL=%s POS_EQUAL=%.6f P1=%.6f P5=%.6f "
                            "P10=%.6f P20=%.6f\n",
                            c.bit ? "YES" : "NO",
                            c.total_cells ? (double)c.equal_cells /
                                            c.total_cells : 0.0,
                            c.p1, c.p5, c.p10, c.p20);
            } else {
                std::printf("BITFINAL=SKIP\n");
            }
        }
        return 0;
    }
    std::fprintf(stderr, "mode %s not implemented yet\n", mode.c_str());
    return 2;
}

// ===========================================================================
// E: F5 via multiply_and_prune (3 products + merge), sparse F6..F9
// ===========================================================================
static M build_csr(idx_t rows, idx_t cols,
                   const std::vector<std::tuple<idx_t, idx_t, float>>& in) {
    M m(rows, cols);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
    size_t chunk = ((size_t)rows + num_threads - 1) / num_threads;
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        idx_t r_start = (idx_t)std::min<size_t>(rows, (size_t)tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(rows, (size_t)(tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
        for (size_t t = 0; t < in.size(); ++t) {
            idx_t r = std::get<0>(in[t]);
            if (r >= r_start && r < r_end) {
                Triplet* tr = allocator.allocate();
                tr->row = r; tr->col = std::get<1>(in[t]);
                tr->value = std::get<2>(in[t]); tr->is_set = false;
            }
        }
    }
    M::build_from_partial_results(m, allocators, ranges);
    return m;
}

// CW from imgCor (row q asc, map keys asc); TP by PATH (Etapa-1 definition).
static M build_CW() {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)leg::n * (size_t)leg::topK);
    for (int q = 0; q < leg::n; ++q)
        for (const auto& kv : leg::imgCor[q])
            trips.emplace_back((idx_t)q, (idx_t)kv.first, kv.second);
    return build_csr((idx_t)leg::n, (idx_t)leg::n, trips);
}
static M build_TP_path() {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)leg::n * (size_t)(leg::topK - 1) *
                  (size_t)(leg::topK - 1));
    for (int q = 0; q < leg::n; ++q) {
        for (int i = 1; i < leg::topK; ++i) {
            int imgI = leg::getRKElem(q, i);
            float ircor = leg::imgCor[q][imgI];
            for (int j = 1; j < leg::topK; ++j) {
                int imgJ = leg::getRKElem(imgI, j);
                float jcor = ircor * leg::imgCor[imgI][imgJ];
                trips.emplace_back((idx_t)q, (idx_t)imgJ, jcor);
            }
        }
    }
    return build_csr((idx_t)leg::n, (idx_t)leg::n, trips);
}

// F6 sparse: colsum sequence identical to legacy (per column, rows ascending;
// zeros are exact no-ops in the double sum) + cell-local divide.
static M normalize_w_prob_sparse(const M& w) {
    const idx_t n = (idx_t)leg::n;
    std::vector<double> colsum(n, 1.0);
    for (idx_t a = 0; a < n; ++a)
        w.for_each_in_row(a, [&](idx_t c, float v) { colsum[c] += v; });
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve(w.nnz());
    for (idx_t a = 0; a < n; ++a)
        w.for_each_in_row(a, [&](idx_t c, float v) {
            trips.emplace_back(a, c, (float)(v / colsum[c]));
        });
    return build_csr(n, n, trips);
}

// F7 sparse: same loops/order as legacy computeWDiffusion; get() misses read
// 0.0f (== the dense zero cell). Accumulation per cell: posM ascending.
static M compute_w_diffusion_sparse(const M& w) {
    const int n = (int)leg::n, l = leg::l;
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)n * (size_t)l);
    for (int imgI = 0; imgI < n; ++imgI) {
        for (int posJ = 0; posJ < l; ++posJ) {
            int imgJ = leg::getRKElem(imgI, posJ);
            float acc = 0.0f;
            for (int posM = 0; posM < l; ++posM) {
                int imgM = leg::getRKElem(imgI, posM);
                acc += w.get((idx_t)imgI, (idx_t)imgM) *
                       w.get((idx_t)imgJ, (idx_t)imgM);
            }
            trips.emplace_back((idx_t)imgI, (idx_t)imgJ, acc);
        }
    }
    return build_csr((idx_t)n, (idx_t)n, trips);
}

// F9 sparse variant: verbatim kernelSortRankedLists with matrix[...] replaced
// by w.get(row, col); sort + pin logic untouched.
static void kernelSortRankedLists_sparse(int rk, const M& w) {
    int l_ = (int)(leg::rkLists.size() / leg::n);
    long int LcurRL = ((long int)l_) * rk;
    float a[l_];
    for (int j = 0; j < l_; j++)
        a[j] = w.get((idx_t)rk, (idx_t)leg::rkLists[LcurRL + j]);
    int i, j, keyR;
    float keyA;
    for (j = 1; j < l_; j++) {
        keyA = a[j];
        keyR = leg::rkLists[LcurRL + j];
        i = j - 1;
        while (i >= 0 && a[i] < keyA) {
            a[i + 1] = a[i];
            leg::rkLists[LcurRL + i + 1] = leg::rkLists[LcurRL + i];
            i--;
        }
        a[i + 1] = keyA;
        leg::rkLists[LcurRL + i + 1] = keyR;
    }
    i = 0;
    while ((leg::rkLists[LcurRL + i] != rk) && (i < l_)) {
        i++;
    }
    if (i > 0) {
        int aux = leg::rkLists[LcurRL + 0];
        leg::rkLists[LcurRL + 0] = leg::rkLists[LcurRL + i];
        leg::rkLists[LcurRL + i] = aux;
        float auxA = a[0];
        a[0] = a[i];
        a[i] = auxA;
    }
}

// E pipeline: F1..F3 verbatim (dense scratch; freed before F5 -- it is a
// shared cost of EVERY variant and would mask E's F5..F9 footprint), then
// SpGEMM W + sparse F6..F9. limit=N never prunes (a row has <= N columns).
static uint64_t run_E(size_t limit, PhaseTimes* t, long* nnz_w_out) {
    const auto t_all = Clock::now();
    auto t0 = Clock::now();
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    t->f1f2 = ms_since(t0);

    t0 = Clock::now();
    leg::computeRankCorrelation();
    t->f3 = ms_since(t0);

    // F4: drop the dense scratch so E's F5..F9 footprint is visible in RSS
    // (the F1/F2 peak itself remains recorded -- common to all variants).
    leg::matrix.clear();
    leg::matrix.shrink_to_fit();

    t0 = Clock::now();
    M CW = build_CW();
    M CW_T = CW.transpose();
    M TP = build_TP_path();
    M TP_T = TP.transpose();
    M P1 = M::multiply_and_prune(CW_T, CW, limit);
    M P2 = M::multiply_and_prune(CW_T, TP, limit);
    M P3 = M::multiply_and_prune(TP_T, TP, limit);
    M we = M::merge(M::merge(P1, P2, [](float a, float b) { return a + b; }),
                    P3, [](float a, float b) { return a + b; });
    t->f5 = ms_since(t0);
    if (nnz_w_out) *nnz_w_out = (long)we.nnz();

    t0 = Clock::now();
    M w6 = normalize_w_prob_sparse(we);          // F6
    t->f6 = ms_since(t0);

    t0 = Clock::now();
    M rc = compute_w_diffusion_sparse(w6);       // F7
    t->f7 = ms_since(t0);

    t0 = Clock::now();
    M rc8 = normalize_w_prob_sparse(rc);         // F8
    t->f8 = ms_since(t0);

    t0 = Clock::now();
    for (int i = 0; i < leg::n; i++)             // F9
        kernelSortRankedLists_sparse(i, rc8);
    t->f9 = ms_since(t0);

    t->total = ms_since(t_all);
    return fnv_rk();
}
// ===========================================================================
// A2: exact row-blocked F5 (inverted index of relevant queries per row/batch)
// + row-blocked F7 (rc emitted CSR in B-row batches). Dense W base stays
// (F7's random reads rk[i] x rk[i] need it -- a full BxN-total sweep was
// priced out during design: per-row replay ~5.7e5 ops x N x (L+1) reads).
// Scratch = B*N floats for F5 replay + B*N for the F7 batch (the knob).
// Bit-exact contract vs A: per cell, adds follow q-asc then the verbatim
// block order b1,b2,b3 with occurrence order inside (same term sequence).
// ===========================================================================
static std::vector<std::vector<uint32_t>> g_inv;  // inv[a] = q's touching row a
static double g_t_inv_ms = 0;

// Membership a in l1(q) U l2(q) (first-index set of q's writes).
static inline bool q_touches_row(int q, int a) {
    if (a == q) return true;
    for (int i = 1; i < leg::topK; i++)
        if (leg::getRKElem(q, i) == a) return true;
    for (int i = 1; i < leg::topK; i++) {
        int imgI = leg::getRKElem(q, i);
        for (int j = 1; j < leg::topK; j++)
            if (leg::getRKElem(imgI, j) == a) return true;
    }
    return false;
}

static void build_inv() {
    const auto t0 = Clock::now();
    const int n = (int)leg::n;
    g_inv.assign(n, {});
    std::vector<int> l1, l2;
    l1.reserve(leg::topK);
    l2.reserve(leg::topK * leg::topK);
    for (int q = 0; q < n; q++) {  // q ascending -> per-row appends asc
        // First-index set of q's writes = set(l1) U set(l2), O(k^2) per q
        // (NOT an N scan per q -- that would be O(N^2 k^2)).
        l1.assign(1, q);
        l2.clear();
        for (int i = 1; i < leg::topK; i++) {
            int imgI = leg::getRKElem(q, i);
            l1.push_back(imgI);
            for (int j = 1; j < leg::topK; j++)
                l2.push_back(leg::getRKElem(imgI, j));
        }
        for (int a : l1) g_inv[a].push_back((uint32_t)q);
        for (int a : l2) g_inv[a].push_back((uint32_t)q);
    }
    for (int a = 0; a < n; a++) {  // same-q dups are adjacent; q-asc overall
        auto& v = g_inv[a];
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    g_t_inv_ms = ms_since(t0);
}

// Replay q's verbatim F5 term sequence into rowbuf, keeping ONLY cells whose
// first index == a. The order of adds per kept cell is the legacy order
// (b1 block, then b2 in (i,j) lex, then b3 in j) -- see test_bfstree_hyp.cpp.
static inline void replay_q_into_row(int q, int a, float* rowbuf) {
    static thread_local std::vector<int> l1, l2;
    static thread_local std::vector<double> l2d;
    l1.clear(); l2.clear(); l2d.clear();
    l1.push_back(q);
    for (int i = 1; i < leg::topK; i++) {
        int imgI = leg::getRKElem(q, i);
        l1.push_back(imgI);
        float ircor = leg::imgCor[q][imgI];
        for (int j = 1; j < leg::topK; j++) {
            int imgJ = leg::getRKElem(imgI, j);
            l2.push_back(imgJ);
            float jcor = ircor * leg::imgCor[imgI][imgJ];
            l2d.push_back(jcor);
        }
    }
    for (int img1a : l1) {
        if (img1a != a) continue;
        for (int img1b : l1)
            rowbuf[img1b] += leg::imgCor[q][img1a] * leg::imgCor[q][img1b];
    }
    for (size_t i = 0; i < l2.size(); i++) {
        if (l2[i] != a) continue;
        for (size_t j = 0; j < l2.size(); j++)
            rowbuf[l2[j]] += (float)(l2d[i] * l2d[j]);
    }
    for (size_t i = 0; i < l1.size(); i++) {
        if (l1[i] != a) continue;
        for (size_t j = 0; j < l2.size(); j++)
            rowbuf[l2[j]] += leg::imgCor[q][l1[i]] * (float)l2d[j];
    }
}

// F5 A2: rows ascending; batch of B rows replayed per pass into B*N scratch
// (parallel inside the batch -- rows are independent), written into dense W.
static void acum_bfstree_a2(long B) {
    const int n = (int)leg::n;
    if (B < 1) B = 1;
    if (B > n) B = n;
    std::vector<float> scratch((size_t)B * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B) {
        const long r1 = std::min<long>(n, r0 + B);
        #pragma omp parallel for schedule(static)
        for (long a = r0; a < r1; a++) {
            float* rowbuf = &scratch[(size_t)(a - r0) * (size_t)n];
            std::fill(rowbuf, rowbuf + n, 0.0f);
            for (uint32_t q : g_inv[a]) replay_q_into_row((int)q, (int)a, rowbuf);
            std::memcpy(&leg::matrix[(size_t)a * (size_t)n], rowbuf,
                        (size_t)n * sizeof(float));
        }
    }
}
// F7 A2: identical loop nest over imgI batches; rc batch scratch B*N,
// emitted as CSR triplets (each rc cell belongs to exactly one imgI, so the
// per-cell posJ/posM accumulation order is preserved batch-independently).
static M compute_w_diffusion_a2(long B) {
    const int n = (int)leg::n, l = leg::l;
    if (B < 1) B = 1;
    if (B > n) B = n;
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)n * (size_t)l);
    std::vector<float> batch((size_t)B * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B) {
        const long r1 = std::min<long>(n, r0 + B);
        #pragma omp parallel
        {
            // Each thread emits the rows it computed (local buffer), then
            // merges under critical -- rows are disjoint and (imgI,imgJ) is
            // unique per row, so emission order cannot change values.
            std::vector<std::tuple<idx_t, idx_t, float>> local;
            #pragma omp for schedule(static)
            for (long imgI = r0; imgI < r1; imgI++) {
                float* rbuf = &batch[(size_t)(imgI - r0) * (size_t)n];
                std::fill(rbuf, rbuf + n, 0.0f);
                for (int posJ = 0; posJ < l; posJ++) {
                    int imgJ = leg::getRKElem(imgI, posJ);
                    float acc = 0.0f;
                    for (int posM = 0; posM < l; posM++) {
                        int imgM = leg::getRKElem(imgI, posM);
                        acc += leg::matrix[(size_t)imgI * n + imgM] *
                               leg::matrix[(size_t)imgJ * n + imgM];
                    }
                    rbuf[imgJ] = acc;
                }
                for (int posJ = 0; posJ < l; posJ++) {
                    int imgJ = leg::getRKElem(imgI, posJ);
                    local.emplace_back((idx_t)imgI, (idx_t)imgJ,
                                       rbuf[imgJ]);
                }
            }
            #pragma omp critical
            {
                trips.insert(trips.end(), local.begin(), local.end());
            }
        }
    }
    return build_csr((idx_t)n, (idx_t)n, trips);
}

// A2 pipeline: verbatim F1..F4, blocked F5 (inv), verbatim dense F6,
// blocked F7 -> CSR, sparse F8/F9 on rc.
static uint64_t run_A2(long B, PhaseTimes* t, long* nnz_w_out,
                       uint64_t* ck_w_out) {
    const auto t_all = Clock::now();
    auto t0 = Clock::now();
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    t->f1f2 = ms_since(t0);

    t0 = Clock::now();
    leg::computeRankCorrelation();
    t->f3 = ms_since(t0);

    t0 = Clock::now();
    leg::zero_matrix();   // F4
    build_inv();          // one-time inverted index (g_t_inv_ms reported)
    acum_bfstree_a2(B);   // F5 blocked exact
    t->f5 = ms_since(t0);  // includes build_inv; T_INV_MS reported separately
    std::printf("T_INV_MS=%.2f\n", g_t_inv_ms);
    long nnz = 0;
    count_density(leg::matrix, &nnz);
    if (nnz_w_out) *nnz_w_out = nnz;
    if (ck_w_out) *ck_w_out = fnv_w(leg::matrix);

    t0 = Clock::now();
    leg::normalizeWProb();  // F6 verbatim on dense W
    t->f6 = ms_since(t0);

    t0 = Clock::now();
    M rc = compute_w_diffusion_a2(B);  // F7 blocked
    t->f7 = ms_since(t0);

    t0 = Clock::now();
    M rc8 = normalize_w_prob_sparse(rc);  // F8 on CSR rc
    t->f8 = ms_since(t0);

    t0 = Clock::now();
    for (int i = 0; i < leg::n; i++)      // F9 sparse getter
        kernelSortRankedLists_sparse(i, rc8);
    t->f9 = ms_since(t0);

    t->total = ms_since(t_all);
    return fnv_rk();
}
// ===========================================================================
// A3: A2 with F6 eliminated.
// Legacy F6 (normalizeWProb): per column i, acum = 1.0 (DOUBLE) + sum_j
// matrix[j][i] (j ascending), then matrix[j][i] = float((double)w / acum).
// A3 keeps the raw W produced by the blocked F5 and:
//  - exact mode : recomputes the IDENTICAL double colsum (per-column work,
//    same ascending-j addition order => bit-exact) -- the O(N^2) division
//    pass disappears entirely;
//  - alg mode   : colsum via sparse vector-matrix products on CW/TP,
//    colsum(W)[i] = 1 + sum_q CW[q][i]*rCW[q] + TP[q][i]*(rCW[q]+rTP[q]),
//    rCW = CW*ones, rTP = TP*ones  (from W = CW^T.CW + CW^T.TP + TP^T.TP)
//    -- O(nnz) = O(N k^2), never touches N^2; differs from legacy colsum
//    only by fp summation order (validated cell-by-cell in mode F6a).
// The division is FUSED into F7 reads as float((double)raw/acum), the exact
// narrowing the legacy stored, so A3-exact stays bit-exact end to end.
// ===========================================================================
static std::vector<double> colsum_legacy_exact() {
    const int n = (int)leg::n;
    std::vector<double> acum((size_t)n, 1.0);
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
        double a = 1.0;
        for (int j = 0; j < n; j++)
            a += (double)leg::matrix[(size_t)j * (size_t)n + (size_t)i];
        acum[(size_t)i] = a;
    }
    return acum;
}

static std::vector<double> colsum_alg(const M& CW, const M& TP) {
    const int n = (int)leg::n;
    std::vector<double> rCW((size_t)n, 0.0), rTP((size_t)n, 0.0);
    #pragma omp parallel for schedule(static)
    for (int q = 0; q < n; q++) {
        double s1 = 0.0, s2 = 0.0;
        CW.for_each_in_row((idx_t)q,
                           [&](idx_t, float v) { s1 += (double)v; });
        TP.for_each_in_row((idx_t)q,
                           [&](idx_t, float v) { s2 += (double)v; });
        rCW[(size_t)q] = s1;
        rTP[(size_t)q] = s2;
    }
    // q-ascending accumulation (deterministic; O(nnz), not O(N^2))
    std::vector<double> acum((size_t)n, 1.0);
    for (int q = 0; q < n; q++) {
        const double c1 = rCW[(size_t)q];
        const double c2 = rCW[(size_t)q] + rTP[(size_t)q];
        CW.for_each_in_row((idx_t)q,
                           [&](idx_t i, float v) { acum[i] += (double)v * c1; });
        TP.for_each_in_row((idx_t)q,
                           [&](idx_t i, float v) { acum[i] += (double)v * c2; });
    }
    return acum;
}

// F7 fused with the F6 division (bit-identical narrowing float(double/acum)).
static M compute_w_diffusion_a3(long B, const std::vector<double>& ac) {
    const int n = (int)leg::n, l = leg::l;
    if (B < 1) B = 1;
    if (B > n) B = n;
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)n * (size_t)l);
    std::vector<float> batch((size_t)B * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B) {
        const long r1 = std::min<long>(n, r0 + B);
        #pragma omp parallel
        {
            std::vector<std::tuple<idx_t, idx_t, float>> local;
            #pragma omp for schedule(static)
            for (long imgI = r0; imgI < r1; imgI++) {
                float* rbuf = &batch[(size_t)(imgI - r0) * (size_t)n];
                std::fill(rbuf, rbuf + n, 0.0f);
                for (int posJ = 0; posJ < l; posJ++) {
                    int imgJ = leg::getRKElem(imgI, posJ);
                    float acc = 0.0f;
                    for (int posM = 0; posM < l; posM++) {
                        int imgM = leg::getRKElem(imgI, posM);
                        const double am = ac[(size_t)imgM];
                        float wi = (float)((double)
                            leg::matrix[(size_t)imgI * n + imgM] / am);
                        float wj = (float)((double)
                            leg::matrix[(size_t)imgJ * n + imgM] / am);
                        acc += wi * wj;
                    }
                    rbuf[imgJ] = acc;
                }
                for (int posJ = 0; posJ < l; posJ++) {
                    int imgJ = leg::getRKElem(imgI, posJ);
                    local.emplace_back((idx_t)imgI, (idx_t)imgJ, rbuf[imgJ]);
                }
            }
            #pragma omp critical
            {
                trips.insert(trips.end(), local.begin(), local.end());
            }
        }
    }
    return build_csr((idx_t)n, (idx_t)n, trips);
}
static uint64_t run_A3(bool alg, int kern, long B, PhaseTimes* t,
                       long* nnz_w_out, uint64_t* ck_w_out) {
    const auto t_all = Clock::now();
    auto t0 = Clock::now();
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    t->f1f2 = ms_since(t0);

    t0 = Clock::now();
    leg::computeRankCorrelation();
    t->f3 = ms_since(t0);

    t0 = Clock::now();
    leg::zero_matrix();   // F4
    build_inv();
    acum_bfstree_a2(B);   // F5 blocked exact -> RAW W in leg::matrix
    t->f5 = ms_since(t0);
    std::printf("T_INV_MS=%.2f\n", g_t_inv_ms);
    long nnz = 0;
    count_density(leg::matrix, &nnz);
    if (nnz_w_out) *nnz_w_out = nnz;
    if (ck_w_out) *ck_w_out = fnv_w(leg::matrix);
    if (std::getenv("BFSTREE_STOP_F5")) {   // scale memprobe: stop after F5
        std::printf("MEMPROBE=afterF5 N=%d RSS_KB=%ld\n", leg::n, rss_kb());
        std::exit(0);
    }
    memprobe("A3_afterF5");

    t0 = Clock::now();
    std::vector<double> acum;
    if (alg) {
        M CW = build_CW();
        M TP = build_TP_path();
        acum = colsum_alg(CW, TP);
    } else {
        acum = colsum_legacy_exact();
    }
    t->f6 = ms_since(t0);   // colsum only -- the division pass no longer exists
    memprobe("A3_afterF6");

    if (kern == 2) {   // A4b: restore the division pass, parallel & in-place
        const int n = (int)leg::n;
        auto tn = Clock::now();
        #pragma omp parallel for schedule(static)
        for (int j2 = 0; j2 < n; j2++) {
            float* row = &leg::matrix[(size_t)j2 * (size_t)n];
            for (int i2 = 0; i2 < n; i2++)
                row[i2] = (float)((double)row[i2] / acum[(size_t)i2]);
        }
        t->f6 += ms_since(tn);   // reported as part of F6 (it IS F6's division)
    }
    t0 = Clock::now();
    M rc = kern == 1 ? compute_w_diffusion_a4<false>(B, acum)
         : kern == 2 ? compute_w_diffusion_a4<true>(B, acum)
                     : compute_w_diffusion_a3(B, acum);   // F7 kernels
    t->f7 = ms_since(t0);
    memprobe("A3_afterF7");

    t0 = Clock::now();
    M rc8 = normalize_w_prob_sparse(rc);      // F8 on CSR rc
    t->f8 = ms_since(t0);

    t0 = Clock::now();
    for (int i = 0; i < leg::n; i++)          // F9 sparse getter
        kernelSortRankedLists_sparse(i, rc8);
    t->f9 = ms_since(t0);

    t->total = ms_since(t_all);
    return fnv_rk();
}

// F6a: cell-by-cell validation of the algebraic colsum vs the legacy double
// colsum (same rigor as the TP formula check): bit-equality, inherited gates
// rel<=5.0e-6 / abs<=4.2e-4, extremes. Exit 0 iff BEYOND==0.
static int run_f6a() {
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    leg::computeRankCorrelation();
    leg::zero_matrix();
    leg::acumBFSTree();
    std::vector<double> leg_ac = colsum_legacy_exact();
    M CW = build_CW();
    M TP = build_TP_path();
    std::vector<double> alg_ac = colsum_alg(CW, TP);
    const int n = (int)leg::n;
    long bit = 0, ok = 0, beyond = 0;
    double max_rel = 0, max_abs = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a, b;
        double x = leg_ac[i], y = alg_ac[i];
        std::memcpy(&a, &x, 8);
        std::memcpy(&b, &y, 8);
        if (a == b) { ++bit; continue; }
        double rel = std::fabs(x - y) / (std::fabs(x) + 1e-30);
        double ad = std::fabs(x - y);
        if (rel > max_rel) max_rel = rel;
        if (ad > max_abs) max_abs = ad;
        if (rel <= 5.0e-6 || ad <= 4.2e-4) ++ok; else ++beyond;
    }
    std::printf("F6A_N=%d BIT=%ld GATE_OK=%ld BEYOND=%ld MAXREL=%.3g "
                "MAXABS=%.3g\n", n, bit, ok, beyond, max_rel, max_abs);
    return beyond == 0 ? 0 : 1;
}
// F7-A4: bandwidth-oriented rewrite of the fused kernel, BIT-EXACT.
// Two changes, neither touches the arithmetic order:
//  1. row-i hoist: wi[m] = float(W[i][rk_i[m]]/acum[rk_i[m]]) depends only on
//     (i, posM), not on posJ -> computed once per row instead of L times;
//  2. sorted gather: row j is read at columns rk_i[] in ASCENDING column order
//     (rising addresses => HW prefetch/TLB friendliness), results scattered
//     into a position-ordered buffer; the dot accumulates over posM ascending
//     exactly like the legacy, so every rc cell is bit-identical.
template <bool NODIV>
static M compute_w_diffusion_a4(long B, const std::vector<double>& ac) {
    const int n = (int)leg::n, l = leg::l;
    if (B < 1) B = 1;
    if (B > n) B = n;
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)n * (size_t)l);
    std::vector<float> batch((size_t)B * (size_t)n, 0.0f);
    for (long r0 = 0; r0 < n; r0 += B) {
        const long r1 = std::min<long>(n, r0 + B);
        #pragma omp parallel
        {
            std::vector<std::tuple<idx_t, idx_t, float>> local;
            std::vector<float> vi((size_t)l), buf((size_t)l);
            std::vector<int> ord((size_t)l);
            #pragma omp for schedule(static)
            for (long imgI = r0; imgI < r1; imgI++) {
                float* rbuf = &batch[(size_t)(imgI - r0) * (size_t)n];
                std::fill(rbuf, rbuf + n, 0.0f);
                const int* rki = &leg::rkLists[(size_t)imgI * (size_t)l];
                // 1. hoisted row-i values (original posM order)
                for (int p = 0; p < l; p++) {
                    const int c = rki[p];
                    vi[(size_t)p] = NODIV
                        ? leg::matrix[(size_t)imgI * n + (size_t)c]
                        : (float)((double)
                              leg::matrix[(size_t)imgI * n + (size_t)c] /
                              ac[(size_t)c]);
                }
                // 2. ascending-column gather order for this row's column set
                for (int p = 0; p < l; p++) ord[(size_t)p] = p;
                std::sort(ord.begin(), ord.end(), [&](int a, int b) {
                    return rki[a] < rki[b]; });
                for (int posJ = 0; posJ < l; posJ++) {
                    const int imgJ = rki[posJ];
                    const float* wj = &leg::matrix[(size_t)imgJ * (size_t)n];
                    for (int t = 0; t < l; t++) {
                        const int p = ord[(size_t)t];
                        const int c = rki[p];
                        buf[(size_t)p] = NODIV
                            ? wj[c]
                            : (float)((double)wj[c] / ac[(size_t)c]);
                    }
                    float acc = 0.0f;
                    for (int p = 0; p < l; p++) acc += vi[(size_t)p] * buf[(size_t)p];
                    rbuf[imgJ] = acc;
                }
                for (int posJ = 0; posJ < l; posJ++)
                    local.emplace_back((idx_t)imgI, (idx_t)rki[posJ],
                                       rbuf[rki[posJ]]);
            }
            #pragma omp critical
            {
                trips.insert(trips.end(), local.begin(), local.end());
            }
        }
    }
    // Byte accounting of the emission transient: this is the part of the A4
    // footprint that is NOT a dense n*n array, and the only part a smarter
    // emitter could remove. Printed always (not only under MEMTRACE) because
    // the scale model in the close README is being re-derived from these.
    std::printf("A4_TRIPS=%zu A4_TRIP_BYTES=%zu A4_TRIP_CAP_BYTES=%zu\n",
                trips.size(), trips.size() * sizeof(trips[0]),
                trips.capacity() * sizeof(trips[0]));
    memprobe("A4_trips_full");
    M rc = build_csr((idx_t)n, (size_t)n ? (idx_t)n : 1, trips);
    std::printf("A4_CSR_NNZ=%llu A4_CSR_BYTES=%llu\n",
                (unsigned long long)rc.nnz(),
                (unsigned long long)(rc.nnz() * (sizeof(idx_t) + sizeof(float)) +
                                     ((size_t)n + 1) * sizeof(idx_t)));
    memprobe("A4_csr_built");
    return rc;
}
// A4 dispatch wired in main.
// NEXT_A4
// ===========================================================================
// Eattr: W-level attribution of E vs A (three error sources, inherited gates)
//   float_ok     : differs but within inherited gate (rel<=5.0e-6 or
//                  abs<=4.2e-4 -- T4a-full numbers, Etapa 1)
//   float_beyond : exceeds the inherited gate (i) -- should be ~0 at limit=N
//   filter       : |A|<=1e-9 and E dropped it (ii) -- limit-independent
//   pruned       : E missing, |A| above filter (iii) -- limit-dependent
//   spurious     : E has a cell A does not
// ===========================================================================
static int run_eattr(size_t limit) {
    PhaseTimes t;
    uint64_t ck_w = 0;
    long nnz_w = 0;
    // Dense A side: verbatim F5 on leg::matrix (front already ran).
    leg::zero_matrix();
    leg::acumBFSTree();
    count_density(leg::matrix, &nnz_w);
    ck_w = fnv_w(leg::matrix);

    // E side: products (same path as run_E's F5).
    M CW = build_CW();
    M CW_T = CW.transpose();
    M TP = build_TP_path();
    M TP_T = TP.transpose();
    M P1 = M::multiply_and_prune(CW_T, CW, limit);
    M P2 = M::multiply_and_prune(CW_T, TP, limit);
    M P3 = M::multiply_and_prune(TP_T, TP, limit);
    M we = M::merge(M::merge(P1, P2, [](float a, float b) { return a + b; }),
                    P3, [](float a, float b) { return a + b; });

    long bit_eq = 0, filt = 0, f_ok = 0, f_beyond = 0, pruned = 0, spur = 0;
    double max_rel_beyond = 0;
    float max_abs_beyond = 0;
    const int n = (int)leg::n;
    const double G_REL = 5.0e-6, G_ABS = 4.2e-4;  // inherited (Etapa 1)
    for (int a = 0; a < n; a++) {
        for (int b = 0; b < n; b++) {
            float av = leg::matrix[(size_t)a * n + b];
            float ev = we.get((idx_t)a, (idx_t)b);
            uint32_t ba, be;
            std::memcpy(&ba, &av, 4);
            std::memcpy(&be, &ev, 4);
            if (ba == be) { ++bit_eq; continue; }
            if (av != 0.0f && ev == 0.0f) {
                if (std::fabs(av) <= 1e-9f) ++filt;
                else ++pruned;
                continue;
            }
            if (av == 0.0f && ev != 0.0f) { ++spur; continue; }
            double rel = std::fabs(double(av) - double(ev)) /
                         (std::fabs(double(av)) + 1e-30);
            float ad = std::fabs(av - ev);
            if (rel <= G_REL || ad <= G_ABS) {
                ++f_ok;
            } else {
                ++f_beyond;
                if (rel > max_rel_beyond) max_rel_beyond = rel;
                if (ad > max_abs_beyond) max_abs_beyond = ad;
            }
        }
    }
    long cells = (long)n * (long)n;
    std::printf("ATTR_CELLS=%ld ATTR_BIT_EQ=%ld ATTR_FILTER=%ld "
                "ATTR_FLOAT_OK=%ld ATTR_FLOAT_BEYOND=%ld ATTR_PRUNED=%ld "
                "ATTR_SPURIOUS=%ld\n",
                cells, bit_eq, filt, f_ok, f_beyond, pruned, spur);
    std::printf("ATTR_MAXREL_BEYOND=%.3g ATTR_MAXABS_BEYOND=%.3g "
                "G_REL=%.3g G_ABS=%.3g\n",
                max_rel_beyond, (double)max_abs_beyond, G_REL, G_ABS);
    std::printf("CK_W_A=0x%016llx NNZ_W_A=%ld NNZ_W_E=%llu\n",
                (unsigned long long)ck_w, nnz_w,
                (unsigned long long)we.nnz());
    return 0;
}
// ===========================================================================
// TOUCH: structural support census of W (F5) + row-cardinality census of
// imgCor (F3). No arithmetic, no W materialized.
//
// Why this exists: rho(W) is the single number that decides whether a sparse W
// representation can ever beat a dense one, and every rho measured in Etapas
// 2-4 came from gen_rklists(), i.e. from RANDOM rank lists. The support is a
// pure function of the rank lists and topK -- the correlation VALUES only scale
// the increments -- so rho can be measured on REAL ALOI structure without
// running F5's arithmetic or materializing W.
//
// W >= 0 with no exceptions: rbo() returns (1-p)*sum p^(k-1)*(inter/k) >= 0 and
// imgCor[q][q] = 1, so every increment in computeIncTree is a product of
// non-negative correlations. No cancellation is possible, hence
//   structural support == non-zero support
// and any gap between TOUCH_SUPPORT and a measured NNZ_W at the same point is
// exact-zero-by-underflow -- the number A5's zero policy needs, measured rather
// than assumed.
//
// TOUCH runs after the F1/F2 re-sort (the order F5 actually sees). TOUCHRAW
// runs on the raw loaded lists and allocates NO n*n array, so it answers the
// rho question at production N even where a dense scratch array cannot fit.
// Comparing the two at the points where both run is what licenses (or refutes)
// the use of TOUCHRAW at that N.
// ===========================================================================
static int run_touch() {
    const int n = (int)leg::n;
    const int k = leg::topK;
    const long n2 = (long)n * (long)n;
    build_inv();

    // imgCor (F3) row-cardinality census -- NA for TOUCHRAW (front not run).
    if (!leg::imgCor.empty() && !leg::imgCor[0].empty()) {
        long cmin = (long)1 << 30, cmax = 0, csum = 0;
        for (int q = 0; q < n; q++) {
            long c = (long)leg::imgCor[q].size();
            cmin = std::min(cmin, c); cmax = std::max(cmax, c); csum += c;
        }
        std::printf("IMG_KEYS_TOTAL=%ld IMG_KEYS_MIN=%ld IMG_KEYS_MAX=%ld "
                    "IMG_KEYS_MEAN=%.3f IMG_RHO=%.6f\n",
                    csum, cmin, cmax, (double)csum / n, (double)csum / n / n);
    } else {
        std::printf("IMG_KEYS=NA (front not run)\n");
    }
    long imin = (long)1 << 30, imax = 0, isum = 0;
    for (int a = 0; a < n; a++) {
        long c = (long)g_inv[a].size();
        imin = std::min(imin, c); imax = std::max(imax, c); isum += c;
    }
    std::printf("INV_TOTAL=%ld INV_MIN=%ld INV_MAX=%ld INV_MEAN=%.3f\n",
                isum, imin, imax, (double)isum / n);

    const auto t0 = Clock::now();
    long support = 0;
    #pragma omp parallel for schedule(dynamic, 1) reduction(+:support)
    for (int a = 0; a < n; a++) {
        static thread_local std::vector<uint64_t> bits;
        static thread_local std::vector<int> l1, l2;
        bits.assign(((size_t)n + 63) >> 6, 0ULL);
        for (uint32_t q : g_inv[a]) {
            // Same l1/l2 construction as computeIncTree / replay_q_into_row.
            l1.clear(); l2.clear();
            l1.push_back((int)q);
            for (int i = 1; i < k; i++) {
                int imgI = leg::getRKElem(q, i);
                l1.push_back(imgI);
                for (int j = 1; j < k; j++)
                    l2.push_back(leg::getRKElem(imgI, j));
            }
            // Row a is written by q through three blocks:
            //   l1 x l1  -> a in l1            marks columns l1
            //   l2 x l2  -> a in l2            marks columns l2
            //   l1 x l2  -> a in l1            marks columns l2
            bool in_l1 = false, in_l2 = false;
            for (int x : l1) if (x == a) { in_l1 = true; break; }
            for (int x : l2) if (x == a) { in_l2 = true; break; }
            if (in_l1)
                for (int x : l1) bits[(size_t)x >> 6] |= 1ULL << (x & 63);
            if (in_l1 || in_l2)
                for (int x : l2) bits[(size_t)x >> 6] |= 1ULL << (x & 63);
        }
        long c = 0;
        for (uint64_t w : bits) c += __builtin_popcountll(w);
        support += c;
    }
    std::printf("TOUCH_N2=%ld TOUCH_SUPPORT=%ld TOUCH_RHO=%.6f "
                "TOUCH_MS=%.2f RSS_KB=%ld\n",
                n2, support, (double)support / (double)n2, ms_since(t0),
                rss_kb());
    return 0;
}
// NEXT_BLOCK











