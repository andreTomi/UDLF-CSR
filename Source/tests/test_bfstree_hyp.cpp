// test_bfstree_hyp.cpp -- Etapa 1 do port BFSTree: verificacao da hipotese
// F5 = SpGEMM via multiply_and_prune (CW = imgCor como CSR).
//
// Hipotese primaria (usuario): bloco l1xl1 do computeIncTree == CW^T . CW.
// Verificada CELULA A CELULA contra o codigo real em N pequeno, dois modos:
//   (a) sintetico : rkLists + imgCor aleatorios (isola a algebra)
//   (b) rbo       : pipeline F1-F3 verbatim reais (RBO, sorts, pin) -- o
//                   "codigo real" com todos os contratos (pinning, keys)
//
// TESTES
//   T1/T3 : bloco l1xl1 verbatim isolado vs mult(CW_T, CW, limit=N)
//           -> exige IGUALDADE DE BITS em todas as N^2 celulas
//           (celulas |v|<=1e-9 caem no filtro de emissao do kernel -- classe
//            "threshold", separada de mismatch; nao deve ocorrer com valores
//            >=0.01 ou RBO>=0.015)
//   T4a   : bloco l2xl2 verbatim vs mult(TP_T, TP); bloco l1xl2 verbatim vs
//           mult(CW_T, TP); F5 completo vs soma dos 3 produtos
//           -> identidade em aritmetica exata; ULPs diferentes esperados
//           (legacy intercala os 3 blocos por query; produtos somam por bloco)
//   T4b   : TP por caminho (verbatim) vs formula fechada (abaixo), rel<=1e-4
//
// FORMULA FECHADA DE TP[q][b] (derivada ANTES do codigo; ponto 1 do revisor):
// Definicao primaria (direta do codigo, BFSTree.cpp:213-219):
//
//   TP[q][b] = SUM_{i=1..k-1} SUM_{j=1..k-1, rk[q'][j]==b, q'=rk[q][i]}
//                  CW[q][q'] * CW[q'][b]
//
//   (exclui q'=q porque o laco 'i' comeca em 1 -- nao ha caminho q->q->b;
//    exclui b=q' porque o laco 'j' comeca em 1 -- nao ha caminho q'->q';
//    cada OCORRENCIA de b em l2 conta como termo distinto.)
//
// Forma fechada (com AMBAS as exclusoes explicitas):
//
//   TP[q][b] = (CW*CW)[q][b]
//            - CW[q][q] * CW[q][b]                                    [exclui q'=q]
//            - 1[b in keys(CW[q]) e b != q] * CW[q][b] * CW[b][b]     [exclui b=q' p/ q'=b]
//
// ATENCAO (correcao ao relatorio inicial): "CW^2 - CW" erra para todo
// b in l1(q)\{q} (saO DUAS subtracoes, nao uma), e os fatores CW[q][q] e
// CW[b][b] NAO sao 1.0 em geral -- com RBO, imgCor[q][q] = rbo(q,q) =
// 1 - p^topK ~ 0.9992 (o laco pos=0 sobrescreve o 1.0 hardcoded). A formula
// so vale com os fatores explicitos. 1[.] por presenca de chave; valores sao
// >=0, entao subtrair termos de valor 0.0 e no-op -- sem ambiguidade.
// E identidade em aritmetica exata mas sequencia de float diferente do
// caminho direto -> T4b mede ULPs, T4a decide viabilidade da E.
//
// CONSTRAINTS ja declarados para a Etapa 2 (registrados aqui):
//   - size_type=uint32_t: E com limit=infinito NAO e executavel na ALOI real
//     (86% * 72000^2 = 4.46e9 > 4.29e9); so variantes podadas (8L/2L/L/k).
//   - Gate de bit-identidade da A2 valera a SAIDA FINAL do pipeline (rc pos-F7,
//     pos-F8, rkLists pos-F9), nao so a W intermediaria.
//   - Etapa 2 deve REPORTAR densidade real medida por ponto de teste e incluir
//     ponto com k^4/N ~ 2.2 (regime ALOI); N=5k-20k com k=20 e regime mais
//     saturado que producao (k^4/N = 32 em N=5000 -> ~100%).
//   - P@k da Etapa 2 isola TRES fontes de erro de E vs A, separadamente:
//       (i)  ruido de ordem de float -- ja medido AQUI no F5 completo:
//            max_rel=5.0e-6, max_abs=4.2e-4 (T4a-full);
//       (ii) corte estrutural do filtro |v|>1e-9 do kernel de emissao
//            (independe do limit; 2 celulas observadas no rbo/seed42 do T3);
//       (iii) poda por limit (depende do limit escolhido).
//     Isolamento: ponto de E com limit >= N (nao poda de fato; teto do
//     uint32_t respeitado) vs denso => (i)+(ii); depois variar limit para
//     baixo e medir o incremento => (iii).
//   - Gates de tolerancia da Etapa 2 HERDAM os numeros medidos nesta etapa
//     (max_rel=5.0e-6, max_abs=4.2e-4 no F5 completo). Aumento de tolerancia
//     exige causa justificada (ex.: acumulo em F6/F7/F8), nao ajuste de folga.
//   - Pin de producao (confirmado no codigo): kernelSortRankedLists re-posiciona
//     o self na posicao 0 incondicionalmente apos CADA sort -- scan linear por
//     valor da propria linha; empates de ordenacao nao afetam a pertinencia
//     (sorts sao permutacoes, membros nunca mudam). Unica pre-condicao: self na
//     linha, garantida pela convencao dos rkLists dos datasets (diagonal
//     d(q,q)=0 na posicao 0) e preservada pelos sorts. Fora desse contrato o
//     legado faz OOB + swap entre linhas -- mesmo modo documentado em
//     test_rdpac_corner.cpp:73, que o port do BFSTree deve rejeitar fail-fast.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
//       Source/tests/test_bfstree_hyp.cpp -o /tmp/test_bfstree_hyp

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <tuple>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ---- public-API CSR construction (same pattern as test_multiply_and_prune) --
static M build_csr(idx_t rows, idx_t cols,
                   const std::vector<std::tuple<idx_t, idx_t, float>>& in) {
    M m(rows, cols);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();

    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
    size_t chunk = (rows + num_threads - 1) / num_threads;

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        idx_t r_start = (idx_t)std::min<size_t>(rows, tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(rows, (tid + 1) * chunk);
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

// ---- float bit/ulp utilities -----------------------------------------------
static inline uint32_t fbits(float f) {
    uint32_t b; std::memcpy(&b, &f, sizeof b); return b;
}
static inline long ulp_dist(float a, float b) {
    if (a == b) return 0;
    if (std::isnan(a) || std::isnan(b)) return 1L << 30;
    uint32_t ba = fbits(a), bb = fbits(b);
    int32_t ia = (ba >> 31) ? ~int32_t(ba) : int32_t(ba);
    int32_t ib = (bb >> 31) ? ~int32_t(bb) : int32_t(bb);
    return std::llabs(int64_t(ia) - int64_t(ib));
}
// ---- comparison stats: dense reference vs sparse candidate ------------------
// Per cell: equal (bit-identical) | threshold (|ref|<=1e-9 & cand 0 -- kernel
// emission filter) | close (rel<=tol -- exact-arithmetic identity with a
// different float tree; allowed only for T4) | mismatch (real divergence).
struct CmpStats {
    long equal = 0, threshold = 0, close = 0, mismatch = 0;
    double max_rel = 0.0;
    long max_ulp = 0;
    float max_abs = 0.0f;
};
static CmpStats compare_dense(const std::vector<float>& ref,
                              const std::vector<float>& cand,
                              double tol_rel, const char* label,
                              size_t n, double abs_floor = 0.0,
                              int max_examples = 5) {
    CmpStats s;
    int shown = 0;
    for (size_t idx = 0; idx < ref.size(); ++idx) {
        float r = ref[idx], c = cand[idx];
        if (fbits(r) == fbits(c)) { ++s.equal; continue; }
        if (std::fabs(r) <= 1e-9f && c == 0.0f) { ++s.threshold; continue; }
        double rel = std::fabs(double(r) - double(c)) /
                     (std::fabs(double(r)) + 1e-30);
        long u = ulp_dist(r, c);
        float ad = std::fabs(r - c);
        if (rel > s.max_rel) s.max_rel = rel;
        if (u > s.max_ulp) s.max_ulp = u;
        if (ad > s.max_abs) s.max_abs = ad;
        // close = same value in exact arithmetic. abs_floor covers the
        // catastrophic-cancellation regime of the T4b closed form: its
        // absolute error is bounded by a few ulp of the O(1..20)
        // intermediates (~2e-6), independent of the cell's own magnitude.
        if (rel <= tol_rel || ad <= abs_floor) { ++s.close; continue; }
        ++s.mismatch;
        if (shown < max_examples) {
            std::printf("    ex[%s] row=%zu col=%zu ref=%.9g cand=%.9g "
                        "rel=%.3g ulp=%ld\n",
                        label, idx / n, idx % n, r, c, rel, u);
            ++shown;
        }
    }
    return s;
}
static void print_stats(const char* label, const CmpStats& s) {
    std::printf("  [%s] equal=%ld threshold=%ld close=%ld mismatch=%ld "
                "max_rel=%.3g max_ulp=%ld max_abs=%.3g\n",
                label, s.equal, s.threshold, s.close, s.mismatch, s.max_rel,
                s.max_ulp, (double)s.max_abs);
}

// ===========================================================================
// LEGACY REFERENCE (verbatim from repo-root BFSTree.cpp; per-block gates so
// T1/T4 isolate the three F5 blocks).
// ===========================================================================
namespace leg {

long int n = 0;   // N
int l = 0;        // working list width (rkLists stride)
int topK = 0;     // = k
std::vector<int> rkLists;                  // n*l
std::vector<float> matrix;                 // n*n dense
std::vector<std::map<int, float>> imgCor;  // n maps

int g_block_mask = 0x7;  // bit0=l1xl1, bit1=l2xl2, bit2=l1xl2 (replaces
                          // `if (incType != 1)`; the `incType != 2` block
                          // never executes with incType=2 -> omitted)

void zero_matrix() { std::fill(matrix.begin(), matrix.end(), 0.0f); }

inline int getRKElem(long int query, long int pos) {
    long int index = ((long int)(l * query + pos));
    return rkLists[index];
}
inline float getMatrixElem(std::vector<float>& m, long int i, long int j) {
    long int index = ((long int)n * i + j);
    return m[index];
}
inline void setMatrixElem(std::vector<float>& m, long int i, long int j,
                          float value) {
    long int index = ((long int)n * i + j);
    m[index] = value;
}
inline float minZero(float x, float y) {
    if (x == 0) return y;
    return std::min(x, y);
}
inline void incSimW(int img1, int img2, float newSim) {
    setMatrixElem(matrix, img1, img2,
                  getMatrixElem(matrix, img1, img2) + newSim);
}

// --- sorting (verbatim) ------------------------------------------------------
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
    // INSERTION SORT (verbatim; strict '<' = estable, desc)
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
    // Setting query image at first position (verbatim; reads element before
    // the i<l bound check -- safe: generator guarantees the query id is in
    // its own row, so the loop always exits with i < l)
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
// --- computeRankNormalization (verbatim, types 1 and 2) ---------------------
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

// --- rbo (verbatim; config default metric -- PARAM_BFSTREE_CORRELATION_METRIC) -
// NOTE: rbo(q,q) = 1 - p^topK ~ 0.9992, NOT 1.0 -- the pos=0 pass of
// computeRankCorrelation overwrites the hardcoded imgCor[q][q] = 1.0.
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
float rankCorrelationMeasure(int i1, int i2) {
    return rbo(i1, i2);  // dispatch fixed to RBO; other metrics abort upstream
}

// --- computeRankCorrelation (verbatim) --------------------------------------
void computeRankCorrelation() {
    imgCor.clear();
    imgCor.resize(n);
    for (int qImg = 0; qImg < n; qImg++) {
        imgCor[qImg][qImg] = 1.0;
        for (int pos = 0; pos < topK; pos++) {
            int img = getRKElem(qImg, pos);
            float correlation = rankCorrelationMeasure(qImg, img);
            imgCor[qImg][img] = correlation;
        }
    }
}

// --- computeIncTree (verbatim; `if (incType != 1)` -> per-block mask) -------
void computeIncTree(int qImg) {
    // const int incType = 2;  // the `incType != 2` early increments are dead
    //                          // code under verbatim incType=2 -> omitted
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
            float jcor = ircor * imgCor[imgI][imgJ];  // verbatim: FLOAT multiply
            l2d.push_back(jcor);
        }
    }

    if (g_block_mask & 0x1) {  // BLOCK 1: l1 x l1
        for (int img1a : l1) {
            for (int img1b : l1) {
                incSimW(img1a, img1b,
                        imgCor[qImg][img1a] * imgCor[qImg][img1b]);
            }
        }
    }
    if (g_block_mask & 0x2) {  // BLOCK 2: l2 x l2 (occurrence pairs)
        for (size_t i = 0; i < l2.size(); i++) {
            for (size_t j = 0; j < l2.size(); j++) {
                incSimW(l2[i], l2[j], l2d[i] * l2d[j]);
            }
        }
    }
    if (g_block_mask & 0x4) {  // BLOCK 3: l1 x l2
        for (size_t i = 0; i < l1.size(); i++) {
            for (size_t j = 0; j < l2.size(); j++) {
                incSimW(l1[i], l2[j],
                        imgCor[qImg][l1[i]] * l2d[j]);
            }
        }
    }
}
void acumBFSTree() {
    for (int qimg = 0; qimg < n; qimg++) computeIncTree(qimg);
}

}  // namespace leg
// ===========================================================================
// Setup helpers: synthetic rkLists, synthetic/real imgCor, CW and TP builders
// ===========================================================================
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
        // CONTRACT (real datasets: self distance 0 -> rank 0): the query id
        // must sit INSIDE the working window, at position 0. Without this the
        // legacy pin loop scans past the row end (verbatim OOB read) and l1
        // can duplicate q -- both off-contract for the hypothesis.
        auto it = std::find(perm.begin(), perm.end(), i);
        std::swap(perm[0], *it);
        for (int j = 0; j < L; ++j)
            leg::rkLists[(size_t)i * L + j] = perm[j];
    }
}
// Mode (a): imgCor keys = l1(q) by construction, values in [0.01, 1]
// (keeps every product/sum far above the kernel's 1e-9 emission filter).
static void gen_imgcor_synthetic(unsigned seed) {
    leg::imgCor.clear();
    leg::imgCor.resize(leg::n);
    std::mt19937 rng(seed ^ 0x9e3779b9u);
    std::uniform_real_distribution<float> vd(0.01f, 1.0f);
    for (int q = 0; q < leg::n; ++q) {
        leg::imgCor[q][q] = vd(rng);
        for (int i = 1; i < leg::topK; ++i)
            leg::imgCor[q][leg::getRKElem(q, i)] = vd(rng);
    }
}
// Mode (b): the real F1-F3 pipeline (normalization sorts + pin + RBO).
static void setup_rbo_pipeline() {
    leg::zero_matrix();
    leg::computeRankNormalization(1);
    leg::computeRankNormalization(2);
    leg::computeRankCorrelation();
}

// CW: one triplet per imgCor entry (row q asc, map keys asc).
static M build_CW() {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)leg::n * (size_t)leg::topK);
    for (int q = 0; q < leg::n; ++q)
        for (const auto& kv : leg::imgCor[q])
            trips.emplace_back((idx_t)q, (idx_t)kv.first, kv.second);
    return build_csr((idx_t)leg::n, (idx_t)leg::n, trips);
}
// TP by PATH (the primary definition from the code): duplicate (q,b) records
// sum in emission order (i asc, j asc) -- one row's records are contiguous,
// so per-row order survives any thread partition of the builder.
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
static std::vector<float> csr_to_dense(const M& m) {
    std::vector<float> d((size_t)leg::n * (size_t)leg::n, 0.0f);
    for (int r = 0; r < leg::n; ++r)
        m.for_each_in_row((idx_t)r, [&](idx_t c, float v) {
            d[(size_t)r * (size_t)leg::n + c] = v;
        });
    return d;
}
static std::vector<float> run_legacy_block(int mask) {
    leg::g_block_mask = mask;
    leg::zero_matrix();
    leg::acumBFSTree();
    return leg::matrix;
}
// ===========================================================================
// The suite
// ===========================================================================
static void run_suite(const char* mode, int seed, bool synthetic) {
    const int N = 200, L = 60, k = 20;
    gen_rklists(seed, N, L);
    leg::topK = k;
    if (synthetic) {
        gen_imgcor_synthetic((unsigned)seed);
    } else {
        setup_rbo_pipeline();
    }
    std::printf("== mode=%s seed=%d N=%d L=%d k=%d\n", mode, seed, N, L, k);

    // Contract checks: pinning and key-set identity keys(CW[q]) == l1(q).
    int pin_ok = 0, keys_ok = 0;
    for (int q = 0; q < N; ++q) {
        if (leg::rkLists[(size_t)q * L] == q) ++pin_ok;
        std::vector<int> l1;
        l1.push_back(q);
        for (int i = 1; i < k; ++i) l1.push_back(leg::getRKElem(q, i));
        std::vector<int> keys;
        for (const auto& kv : leg::imgCor[q]) keys.push_back(kv.first);
        std::sort(l1.begin(), l1.end());
        std::sort(keys.begin(), keys.end());
        if (l1 == keys) {
            ++keys_ok;
        } else if (q < 3) {
            std::printf("    keys mismatch q=%d l1size=%zu keysize=%zu\n", q,
                        l1.size(), keys.size());
        }
    }
    std::printf("  pin_ok=%d/%d keys_ok=%d/%d\n", pin_ok, N, keys_ok, N);
    if (!synthetic) CHECK(pin_ok == N, "pinning failed (rk[q][0] != q)");
    CHECK(keys_ok == N, "imgCor keys != l1(q) -- hypothesis precondition");

    // --- T3: block l1xl1 verbatim vs CW^T * CW (BIT equality required) -----
    std::vector<float> W_ref = run_legacy_block(0x1);
    M CW = build_CW();
    M CW_T = CW.transpose();
    M P1 = M::multiply_and_prune(CW_T, CW, (size_t)N);  // limit=N: no prune
    std::vector<float> D1 = csr_to_dense(P1);
    CmpStats t3 = compare_dense(W_ref, D1, 1e-4, "T3", (size_t)N);
    print_stats("T3 l1xl1 vs CW_T*CW", t3);
    CHECK(t3.mismatch == 0, "T3: real value mismatch");
    CHECK(t3.close == 0, "T3: differs within tol -> NOT bit-identical");
    // threshold (|ref|<=1e-9, kernel dropped it) is ENGINE FILTER, not the
    // hypothesis -- reported as data for E's F7 lookups, not a failure.
    if (t3.threshold > 0)
        std::printf("    note: %ld cells <=1e-9 dropped by kernel filter\n",
                    t3.threshold);
    // --- T4a: blocks 2/3/full vs path-TP products (exact-arith identity) ---
    M TP = build_TP_path();
    M TP_T = TP.transpose();

    std::vector<float> W2_ref = run_legacy_block(0x2);
    M P2 = M::multiply_and_prune(TP_T, TP, (size_t)N);
    CmpStats t42 = compare_dense(W2_ref, csr_to_dense(P2), 1e-3, "T4a-2",
                                 (size_t)N, 1e-5);
    print_stats("T4a l2xl2 vs TP_T*TP", t42);
    CHECK(t42.mismatch == 0, "T4a: block2 value mismatch beyond tol");

    std::vector<float> W3_ref = run_legacy_block(0x4);
    M P3 = M::multiply_and_prune(CW_T, TP, (size_t)N);
    CmpStats t43 = compare_dense(W3_ref, csr_to_dense(P3), 1e-3, "T4a-3",
                                 (size_t)N, 1e-5);
    print_stats("T4a l1xl2 vs CW_T*TP", t43);
    CHECK(t43.mismatch == 0, "T4a: block3 value mismatch beyond tol");

    std::vector<float> WF_ref = run_legacy_block(0x7);
    std::vector<float> Dsum = csr_to_dense(P1);
    std::vector<float> D2 = csr_to_dense(P2), D3 = csr_to_dense(P3);
    for (size_t i = 0; i < Dsum.size(); ++i) Dsum[i] += (D2[i] + D3[i]);
    CmpStats t4f = compare_dense(WF_ref, Dsum, 1e-3, "T4a-full", (size_t)N,
                                 1e-5);
    print_stats("T4a full F5 vs P1+P2+P3", t4f);
    CHECK(t4f.mismatch == 0, "T4a: full-F5 value mismatch beyond tol");

    // --- T4b: closed-form TP vs path TP ------------------------------------
    M CW2 = M::multiply_and_prune(CW, CW, (size_t)N);
    std::vector<float> tp_path = csr_to_dense(TP);
    std::vector<float> tp_cf = csr_to_dense(CW2);  // start from (CW*CW)[q][b]
    for (int q = 0; q < N; ++q) {
        float cqq = CW.get((idx_t)q, (idx_t)q);
        for (int b = 0; b < N; ++b) {
            float cqb = CW.get((idx_t)q, (idx_t)b);
            float sub1 = cqq * cqb;                 // exclui q' = q
            float sub2 = (q == b) ? 0.0f
                                  : cqb * CW.get((idx_t)b, (idx_t)b);
            tp_cf[(size_t)q * N + b] -= (sub1 + sub2);
        }
    }
    CmpStats t4b = compare_dense(tp_path, tp_cf, 1e-3, "T4b", (size_t)N,
                                 1e-5);
    print_stats("T4b path TP vs closed form", t4b);
    CHECK(t4b.mismatch == 0, "T4b: closed-form TP mismatch beyond tol");
}

int main() {
    std::printf("Test suite: BFSTree F5=SpGEMM hypothesis (threads=%d)\n",
                omp_get_max_threads());
    const int seeds[3] = {42, 7, 123};
    for (int sidx = 0; sidx < 3; ++sidx) {
        run_suite("synthetic", seeds[sidx], true);
        run_suite("rbo", seeds[sidx], false);
    }
    std::printf("RESULT: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}




