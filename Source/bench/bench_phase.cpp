// bench_phase.cpp
//
// Phase 0 profiling driver for CSRMatrix
// (Source/src/Core/hpp/SparseMatrix.hpp).
//
// Runs the production pipeline on synthetic matrices shaped like the UDLF
// workloads and prints machine-parseable lines:
//
//   RUN,<mode>,<workload>,<omp_threads>,<reps>,<phase_profile_flag>
//   REP,<i>,<total_ms>,<nnz>,<checksum>          one per repetition
//   REPRU,<i>,<minflt_delta>,<majflt_delta>,<maxrss_kb>
//                                                per-rep resource delta (S1-bis:
//                                                tells a first-touch/cold-start
//                                                penalty from a real slowdown)
//   PHASEPROFILE,...                             per-phase CSV (only when
//                                                built with -DPHASE_PROFILE)
//   DONE,<workload>,<vmhwm_kb>,<ru_maxrss_kb>
//
// Aggregation (median over reps, rep 0 discarded) is done offline by
// Source/bench/aggregate_phase.py.
//
// Workloads (uniform random k-sparse; kNN-clustered columns are a known
// limitation -- Phase 0 uses these only to exercise both accumulator
// branches and the heavy-prune regime):
//   w1: CPRR-like, DENSE accumulator branch (B.cols/bound = 50000/400 = 125
//       < ADAPTIVE_RATIO 200), prune limit 50.
//   w2: CPRR-like, HASH accumulator branch   (B.cols/bound = 1e6/400 = 2500),
//       prune limit 50.
//   w3: LHRR-like heavy prune: bound ~= ra*rb = 62500/row, limit 1000
//       (dense branch; nth_element dominates).
//   w4: HASH branch with an L1-busting table (bound = 65536 slots, 512 KB):
//       the only regime where contiguous-pair materialization could pay off.
//
// Usage (A0 extends the Phase 0 CLI; old invocations still work):
//   bench_phase mult   <w1|w2|w3|w4> [reps=7]
//   bench_phase pipe   <w1|w2|w3|w4> [reps=7]
//   bench_phase params [w1|w2|w3|w4]           workload sheet (bound/ratio)
//   bench_phase extract <rkLists.txt> <N> <L> <out.bin>
//   bench_phase real <bin> <N> <L> <k> <limit> <cprr|lhrr> [reps=7]
//   mult: reps x multiply_and_prune(A, B, limit)
//   pipe: reps x { WT=transpose(A); MA=mult(WT,A,limit);
//                  MB=mult(A,WT,limit); C=merge(MA,MB,plus) }  (CPRR iteration)
//   real/lhrr: position matrix + T=1 iteration (H build, transpose, 2x mult,
//              2x combine), exactly as LHRRSparse does for the first pass.
// Threads: OMP_NUM_THREADS / PHASE_THREADS. S2.3 step 2 removed the
// std::execution::par_unseq build sort, so OpenMP is the only thread source
// and no TBB arena is created or configured. The effective affinity mask is
// printed on the TSET line.
// Memory sampling: PHASE_MEM=1 enables the per-phase RSS/HWM points and the
// per-build ledger (with /proc/self/clear_refs reset before each build).
//
// Build (WSL, from repo root):
//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp \
//       -o /tmp/bench_phase
//   add -DPHASE_PROFILE for the per-phase report.

#include "Core/hpp/SparseMatrix.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <tuple>
#include <unistd.h>
#include <vector>

using M = CSRMatrix<float>;
using idx_t = M::size_type;
using Triplet = M::Triplet;

// ---------------------------------------------------------------------------
// A0 helpers: wall clock, per-statement ledger, affinity reporting.
// ---------------------------------------------------------------------------
static double now_ms() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch())
        .count();
}

// Statement-level timers of the driver (the engine reports its own phases).
// One row per (rep, label); medians are printed at the end of the run.
struct DrvLedger {
    std::vector<std::pair<std::string, double>> samples;
    void add(const char* label, double ms) { samples.emplace_back(label, ms); }
    void report() const {
        // Stable order = first appearance; median over all reps.
        std::vector<std::string> labels;
        for (const auto& s : samples)
            if (std::find(labels.begin(), labels.end(), s.first) == labels.end())
                labels.push_back(s.first);
        for (const auto& lab : labels) {
            std::vector<double> v;
            for (const auto& s : samples) if (s.first == lab) v.push_back(s.second);
            std::sort(v.begin(), v.end());
            double med = v.empty() ? 0.0 : v[v.size() / 2];
            std::printf("DRVAGG,%s,median_ms=%.3f,n=%zu\n", lab.c_str(), med, v.size());
        }
    }
};

static std::string affinity_mask() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return "?";
    std::string out;
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &set)) continue;
        if (n++) out += ",";
        out += std::to_string(c);
    }
    return out;
}

// Mask captured at process entry. Kept for the record, but note that it is NOT
// a reliable reading of the taskset mask: libgomp applies OMP_PROC_BIND /
// OMP_PLACES in a library constructor, i.e. BEFORE main() runs, so with
// OMP_PROC_BIND=true + OMP_PLACES=cores the initial thread is already re-pinned
// to its place ("0,1" for core 0) by the time this executes. The authoritative
// mask check lives in the runner (`taskset -c $cpus nproc`), which measures the
// mask outside the OMP process; `req_mask` below echoes the requested mask so
// each capture is self-documenting.
static std::string g_affinity_at_entry;

static std::string env_or(const char* key, const char* dflt) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string(dflt);
}

static void print_tset(int omp_threads) {
    // tbb_max is gone (S2.3 p2: the builder no longer sorts through
    // std::execution/par_unseq). The field is kept as an explicit "none" so an
    // older parser fails loudly on the value instead of reading a stale
    // number; max_concurrency now reports the OpenMP pool.
    std::printf("TSET,omp_threads=%d,tbb_max=none,affinity_omp=%s,"
                "affinity_at_entry=%s,req_mask=%s,"
                "cpus=%ld,max_concurrency=%zu,pid=%d\n",
                omp_threads, affinity_mask().c_str(),
                g_affinity_at_entry.c_str(), env_or("PHASE_MASK", "unset").c_str(),
                sysconf(_SC_NPROCESSORS_ONLN),
                (size_t)omp_get_max_threads(), (int)getpid());
}

struct Workload {
    const char* name;
    idx_t rows_a;   // rows of A
    idx_t inner;    // cols of A == rows of B
    idx_t cols_b;   // cols of B
    int ra, rb;     // nnz per row of A / B
    size_t limit;   // prune limit
};

static const Workload WORKLOADS[] = {
    // name  rows_a  inner   cols_b     ra   rb  limit
    {"w1",   50000, 100000,   50000,    20,  20,   50},
    {"w2",   50000, 100000, 1000000,    20,  20,   50},
    {"w3",   20000, 100000,  100000,   250, 250, 1000},
    // w4: HASH branch whose table no longer fits L1d -- the one regime where
    //     materializing contiguous (col, val) pairs could plausibly beat the
    //     indirect acc.val_of(slot) comparator. bound = 256*256 = 65536 slots
    //     => slot_val (u32) + slot_col (u32) = 512 KB, ~10x L1d and ~2x the
    //     per-core L2 on the reference i5. limit=50 forces nth_element over
    //     the whole 65536-slot row, exactly the phase Fase 3 targeted.
    {"w4",   20000,  20000, 32000000,   256, 256,   50},
};

// Build a CSR matrix through the production Row-Split API.
// (build_into / dump_matrix are defined after validate_wf; forward declaration
// so this earlier call site can use the A/B switch + dump.)
static void build_into(M& m, std::vector<M::PaddedAllocator>& allocators,
                       std::vector<std::pair<idx_t, idx_t>>& ranges,
                       const char* tag);

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
    build_into(m, allocators, ranges, "ab");
    return m;
}

// Random k-sparse CSR, distinct sorted columns per row (uniform columns).
static M random_csr(idx_t rows, idx_t cols, int nnz_per_row, std::mt19937& rng) {
    std::vector<std::tuple<idx_t, idx_t, float>> trips;
    trips.reserve((size_t)rows * nnz_per_row);
    std::uniform_real_distribution<float> valdist(-1.0f, 1.0f);
    for (idx_t r = 0; r < rows; ++r) {
        std::vector<idx_t> cols_row;
        int want = std::min<int>(nnz_per_row, (int)cols);
        std::uniform_int_distribution<idx_t> sample(0, cols - 1);
        while ((int)cols_row.size() < want) {
            idx_t c = sample(rng);
            if (std::find(cols_row.begin(), cols_row.end(), c) == cols_row.end())
                cols_row.push_back(c);
        }
        std::sort(cols_row.begin(), cols_row.end());
        for (idx_t c : cols_row) {
            float v = valdist(rng);
            trips.emplace_back(r, c, v == 0.0f ? 0.5f : v);
        }
    }
    return build_csr(rows, cols, trips);
}

// Deterministic checksum (storage order) -- same definition as bench_e2e.
static double checksum(const M& m) {
    double s = 0.0;
    for (idx_t r = 0; r < m.rows(); ++r)
        m.for_each_in_row(r, [&](idx_t, float v) { s += (double)v; });
    return s;
}

// ---------------------------------------------------------------------------
// Ordem: raw-order validation of pipeline outputs. Enabled with
// PHASE_VALIDATE=1; O(nnz) per call, so use it on dedicated validation
// captures, not on timed runs. Mirrors test_csr_ops.cpp csr_well_formed():
// get() is lower_bound and merge() is a two-pointer, so a row whose columns
// are not strictly increasing silently corrupts the consumers.
// ---------------------------------------------------------------------------
static int g_validate = 0;

static bool csr_cols_ascending(const M& m, const char** why) {
    size_t total = 0;
    for (idx_t r = 0; r < m.rows(); ++r) {
        total += (size_t)m.row_nnz(r);
        idx_t prev = 0;
        bool first = true, ok = true;
        m.for_each_in_row(r, [&](idx_t c, float) {
            if (!first && c <= prev) ok = false;
            prev = c;
            first = false;
        });
        if (!ok) { *why = "columns not strictly increasing in a row"; return false; }
    }
    if (total != m.nnz()) { *why = "row_ptr_ total != nnz()"; return false; }
    return true;
}

static void validate_wf(const M& m, const char* what) {
    const char* why = "";
    bool ok = csr_cols_ascending(m, &why);
    std::printf("VALIDATE,%s,%s,%s\n", what, ok ? "ok" : "FAIL", why);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Driver knobs and the canonical dump.
//   PHASE_BUILDER, PHASE_FRAGAB   GONE (S2.3 step 2): the legacy and
//                             fragment-CSR builders were removed, so every
//                             driver-side build is v2. Setting either prints a
//                             warning and is otherwise ignored -- a stale
//                             harness must not silently get the wrong arm.
//   PHASE_ROWSORT=idx|zip     v2 per-row sort strategy
//   PHASE_DUMP_DIR=<dir>      write canonical dumps of every driver-side build
//                             (rep 0 only)
//   PHASE_RK_OUT=<file>       run_real only: after the last rep, write the
//                             final reranked lists as raw row-major u32 (N*L),
//                             for rk_cmp.py (layer-0 legacy-vs-v2 comparison)
//
// Why the dump exists: the driver `checksum` is a weak invariant (a double
// accumulation ordered by column: it cannot see intra-row order or a value
// swap). The canonical dump is row-major with the raw value bits, so md5sum
// over it is the strong equivalence check the S2 gate requires.
// ---------------------------------------------------------------------------
static std::string g_dump_dir;
static int g_dump_now = 0;
static std::string g_rk_out;   // PHASE_RK_OUT: final rk lists (raw u32) sink

static void dump_matrix(const M& m, const char* tag) {
    if (!g_dump_now || g_dump_dir.empty()) return;
    std::string path = g_dump_dir;
    path += "/";
    path += tag;
    path += ".bin";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "dump: cannot open %s\n", path.c_str());
        return;
    }
    uint64_t hdr[3] = {(uint64_t)m.rows(), (uint64_t)m.cols(), (uint64_t)m.nnz()};
    std::fwrite(hdr, sizeof(uint64_t), 3, f);
    // Self-describing layout, so an external reader (dump_cmp.py) can parse it:
    //   rows, cols, nnz (u64) | row_ptr rows+1 (u64) | per row: col u32, val u32
    std::vector<uint64_t> rp(m.rows() + 1);
    uint64_t acc = 0;
    for (idx_t r = 0; r < m.rows(); ++r) {
        rp[r] = acc;
        acc += (uint64_t)m.row_nnz(r);
    }
    rp[m.rows()] = acc;
    std::fwrite(rp.data(), sizeof(uint64_t), rp.size(), f);
    std::vector<uint32_t> buf;                 // 2 entries per nnz
    for (idx_t r = 0; r < m.rows(); ++r) {
        buf.clear();
        m.for_each_in_row(r, [&](idx_t c, float v) {
            uint32_t cb = (uint32_t)c;
            uint32_t vb;
            std::memcpy(&vb, &v, sizeof(vb));
            buf.push_back(cb);
            buf.push_back(vb);
        });
        if (!buf.empty()) std::fwrite(buf.data(), sizeof(uint32_t), buf.size(), f);
    }
    std::fclose(f);
    std::printf("DUMP,%s,nnz=%zu\n", tag, m.nnz());
}

// Single entry point for the driver's own builds, so neither the A/B switch nor
// the dump can be forgotten at a call site.
// S2.3 step 2: the driver-side builder is the single v2 path
// (build_from_partial_results). PHASE_BUILDER/PHASE_FRAGAB and the
// phase_ab.hpp interleave were removed with the legacy and fragment-CSR
// builders; the canonical dump below is unchanged.
static void build_into(M& m, std::vector<M::PaddedAllocator>& allocators,
                       std::vector<std::pair<idx_t, idx_t>>& ranges,
                       const char* tag) {
    M::build_from_partial_results(m, allocators, ranges);
    dump_matrix(m, tag);
}

// ---------------------------------------------------------------------------
// A0/A5: real-data support (rkLists text -> u32 binary -> W / H matrices).
//
// The text parser mirrors VirtualArray::assignFromTextFile + the methods: a
// rank list row is used as a *prefix* of its file line, in file order. The
// binary form exists so repeated profiling runs never re-read the multi-GB
// text file.
// ---------------------------------------------------------------------------
static double nnz_per_row_of(const M& m) {
    if (m.rows() == 0) return 0.0;
    return (double)m.nnz() / (double)m.rows();
}

static void print_params(const Workload& W, const M& A, const M& B) {
    // Measured nnz/row + the real fan-out bound of the multiply:
    // bound_i = sum_{k in row_i(A)} nnz_B(k). Uniform synthetic inputs give
    // ra*rb; rkLists-derived matrices do not, so mean/max are measured.
    double bound_sum = 0.0, bound_max = 0.0;
    for (idx_t i = 0; i < A.rows(); ++i) {
        double b = 0.0;
        A.for_each_in_row(i, [&](idx_t k, float) { b += (double)B.row_nnz(k); });
        bound_sum += b;
        if (b > bound_max) bound_max = b;
    }
    double bound_mean = A.rows() ? bound_sum / (double)A.rows() : 0.0;
    double ratio = bound_max > 0.0 ? (double)B.cols() / bound_max : 0.0;
    std::printf("PARAMS,name=%s,rows_a=%u,inner=%u,cols_b=%u,limit=%zu,"
                "nnz_a=%zu,nnz_b=%zu,nnz_per_row_a=%.3f,nnz_per_row_b=%.3f,"
                "bound_mean=%.3f,bound_max=%.0f,ratio=%.3f,branch=%s,"
                "adaptive_ratio=%d\n",
                W.name, A.rows(), A.cols(), B.cols(), W.limit,
                A.nnz(), B.nnz(), nnz_per_row_of(A), nnz_per_row_of(B),
                bound_mean, bound_max, ratio,
                ratio > 200.0 ? "hash" : "dense", 200);
}

// Streams the text file once: takes the first `L` tokens of each of the first
// `N` lines into an N*L uint32 buffer. Fails loudly on short/ragged lines or
// ids outside [0, N) -- an out-of-range column would corrupt the dense
// accumulator (marker[col]) instead of raising an error.
static std::vector<uint32_t> extract_rk(const char* in, size_t N, size_t L) {
    std::ifstream f(in);
    if (!f) throw std::runtime_error(std::string("cannot open ") + in);
    std::vector<uint32_t> out(N * L);
    std::string line;
    uint32_t max_id = 0;
    size_t min_tok = (size_t)-1;
    double t0 = now_ms();
    for (size_t r = 0; r < N; ++r) {
        if (!std::getline(f, line))
            throw std::runtime_error("not enough lines in input file");
        const char* p = line.c_str();
        size_t tok = 0;
        while (tok < L) {
            while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
            if (!*p) break;
            char* endp = nullptr;
            unsigned long v = std::strtoul(p, &endp, 10);
            if (endp == p) throw std::runtime_error("non-numeric token in rkLists");
            if (v >= N)
                throw std::runtime_error("rkLists id " + std::to_string(v) +
                                         " >= N=" + std::to_string(N) +
                                         " (row " + std::to_string(r) + ")");
            out[r * L + tok++] = (uint32_t)v;
            if (v > max_id) max_id = (uint32_t)v;
            p = endp;
        }
        if (tok < min_tok) min_tok = tok;
        if (tok < L)
            std::fprintf(stderr,
                         "[extract] warning: row %zu has only %zu of %zu tokens\n",
                         r, tok, L);
    }
    std::printf("EXTRACT,in=%s,rows=%zu,cols=%zu,min_tokens=%zu,max_id=%u,"
                "elapsed_ms=%.1f,bytes_out=%zu\n",
                in, N, L, min_tok, max_id, now_ms() - t0, N * L * 4);
    return out;
}

static std::vector<uint32_t> read_rk_bin(const char* path, size_t N, size_t L) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    std::vector<uint32_t> out(N * L);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(N * L * 4));
    if (!f) throw std::runtime_error("short read from rk binary (wrong N/L?)");
    return out;
}

// Weight mirrors the methods: CPRR uses w = k - j (rank position weight),
// LHRR uses weightPosition(pos) = 1 - log(pos)/log(k).
static inline float cprr_weight(int k, int j) { return (float)(k - j); }
static inline float lhrr_weight(int pos, float logK) {
    return (pos == 1) ? 1.0f : (1.0f - std::log((float)pos) / logK);
}

// W[i][rk[i*L+j]] += k - j   (CprrSparse.hpp:63-74), Row-Split + allocators.
static M build_w_from_rk(const std::vector<uint32_t>& rk, idx_t N, idx_t L, int k) {
    M W(N, N, 0);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        size_t chunk = (N + num_threads - 1) / num_threads;
        idx_t r_start = (idx_t)std::min<size_t>(N, tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(N, (tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
        for (idx_t i = r_start; i < r_end; ++i) {
            for (int j = 0; j < k; ++j) {
                uint32_t neighbor = rk[(size_t)i * L + j];
                Triplet* t = allocator.allocate();
                t->row = i; t->col = neighbor;
                t->value = cprr_weight(k, j); t->is_set = false;
            }
        }
    }
    build_into(W, allocators, ranges, "W");
    return W;
}

// Position matrix (LHRRSparse.hpp:50-81): symmetric, w = L - j, emitted with
// `omp for schedule(dynamic)`, so rows land OUTSIDE the owner's range and the
// global (sorted) routing path is exercised -- the Phase-1 exception case.
static M build_position_from_rk(const std::vector<uint32_t>& rk, idx_t N, idx_t L) {
    M MM(N, N, 0);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        size_t chunk = (N + num_threads - 1) / num_threads;
        idx_t r_start = (idx_t)std::min<size_t>(N, tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(N, (tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
#pragma omp for schedule(dynamic)
        for (int i = 0; i < (int)N; ++i) {
            for (int j = 0; j < (int)L; ++j) {
                uint32_t neighbor = rk[(size_t)i * L + j];
                float w = (float)(L - j);
                Triplet* t1 = allocator.allocate();
                t1->row = i; t1->col = neighbor; t1->value = w; t1->is_set = false;
                Triplet* t2 = allocator.allocate();
                t2->row = neighbor; t2->col = i; t2->value = w; t2->is_set = false;
            }
        }
    }
    build_into(MM, allocators, ranges, "MM");
    return MM;
}

// Hyperedge matrix H (LHRRSparse.hpp:107-126): self weight 1 plus k*k entries.
static M build_h_from_rk(const std::vector<uint32_t>& rk, idx_t N, idx_t L, int k) {
    M H(N, N, 0);
    float logK = std::log((float)k);
    int num_threads = omp_get_max_threads();
    std::vector<M::PaddedAllocator> allocators;
    allocators.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) allocators.emplace_back();
    std::vector<std::pair<idx_t, idx_t>> ranges(num_threads);
#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        auto& allocator = *allocators[tid].ptr;
        size_t chunk = (N + num_threads - 1) / num_threads;
        idx_t r_start = (idx_t)std::min<size_t>(N, tid * chunk);
        idx_t r_end   = (idx_t)std::min<size_t>(N, (tid + 1) * chunk);
        ranges[tid] = {r_start, r_end};
        for (idx_t i = r_start; i < r_end; ++i) {
            Triplet* t = allocator.allocate();
            t->row = i; t->col = i; t->value = 1.0f; t->is_set = false;
            for (int o = 0; o < k; ++o) {
                uint32_t imgO = rk[i * L + o];
                float wO = lhrr_weight(o + 1, logK);
                for (int j = 0; j < k; ++j) {
                    uint32_t imgJ = rk[(size_t)imgO * L + j];
                    float wJ = lhrr_weight(j + 1, logK);
                    Triplet* t2 = allocator.allocate();
                    t2->row = i; t2->col = imgJ; t2->value = wO * wJ;
                    t2->is_set = false;
                }
            }
        }
    }
    build_into(H, allocators, ranges, "H");
    return H;
}

// updateRankedLists (CprrSparse.hpp:99-125 / LHRRSparse.hpp:146-168): per-row
// sort by score desc, take L, force self at position 0. Driver-level phase
// (F6): it lives in the method, not in the engine, but it is part of the real
// pipeline and is absent from the synthetic pipe workload.
static void update_ranked_lists(const M& mat, std::vector<uint32_t>& rk,
                                idx_t N, idx_t L) {
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < (int)N; ++i) {
        std::vector<std::pair<float, uint32_t>> scores;
        scores.reserve(mat.row_nnz(i));
        mat.for_each_in_row(i, [&](uint32_t col, float val) {
            scores.push_back({val, col});
        });
        std::sort(scores.begin(), scores.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        int limit = std::min((int)scores.size(), (int)L);
        for (int j = 0; j < limit; ++j) rk[(size_t)i * L + j] = scores[j].second;
        int q_pos = -1;
        for (int j = 0; j < limit; ++j)
            if (rk[(size_t)i * L + j] == (uint32_t)i) q_pos = j;
        if (q_pos > 0) {
            std::swap(rk[(size_t)i * L], rk[(size_t)i * L + q_pos]);
        } else if (q_pos == -1) {
            for (int j = (int)L - 1; j > 0; --j)
                rk[(size_t)i * L + j] = rk[(size_t)i * L + j - 1];
            rk[(size_t)i * L] = (uint32_t)i;
        }
    }
}

// ---------------------------------------------------------------------------
// A5: real-data pipeline runner. `lhrr` = position matrix + one iteration
// (H build, transpose, 2x mult, 2x combine); `cprr` = W build, transpose,
// 2x mult, merge. Each rep restores the input rank lists so every repetition
// measures the identical workload (the method itself would carry state across
// iterations; the reps here are samples, not iterations).
// ---------------------------------------------------------------------------
static int run_real(const char* dataset, const char* bin, size_t N, size_t L,
                    int k, size_t limit, const char* pipe, int reps,
                    int omp_threads, int pp_flag) {
    std::printf("RUN,real-%s,%s,%d,%d,%d\n", pipe, dataset, omp_threads, reps,
                pp_flag);
    print_tset(omp_threads);
    std::printf("REAL,dataset=%s,bin=%s,N=%zu,L=%zu,k=%d,limit=%zu,T=1,"
                "pipeline=%s\n", dataset, bin, N, L, k, limit, pipe);

    std::vector<uint32_t> rk0 = read_rk_bin(bin, N, L);
    std::vector<uint32_t> rk = rk0;
    DrvLedger drv;
    M result;

    // S1-bis: rusage is cumulative for the process, so keep the previous
    // snapshot to report per-rep deltas (see the REPRU line below).
    struct rusage ru_prev;
    getrusage(RUSAGE_SELF, &ru_prev);

    for (int rep = 0; rep < reps; ++rep) {
#ifdef PHASE_PROFILE
        phprof::reset();
#endif
        g_dump_now = (rep == 0 && !g_dump_dir.empty()) ? 1 : 0;
        double tr = now_ms();
        std::copy(rk0.begin(), rk0.end(), rk.begin());
        drv.add("restore_rk_state", now_ms() - tr);
        double t0 = now_ms();

        if (std::strcmp(pipe, "cprr") == 0) {
            // CprrSparse.hpp:36-93 -- W, WT=transpose(W),
            // MA=mult(WT,W,l), MB=mult(W,WT,l), merge(plus)
            std::unique_ptr<M> Wm, WT, MA, MB;
            { double a = now_ms();
              Wm = std::make_unique<M>(build_w_from_rk(rk, N, L, k));
              drv.add("w_build", now_ms() - a); }
            if (g_dump_now) dump_matrix(*Wm, "W");
            { double a = now_ms();
              WT = std::make_unique<M>(Wm->transpose());
              drv.add("w_transpose", now_ms() - a); }
            { double a = now_ms();
              MA = std::make_unique<M>(M::multiply_and_prune(*WT, *Wm, limit));
              drv.add("mult_A", now_ms() - a); }
            { double a = now_ms();
              MB = std::make_unique<M>(M::multiply_and_prune(*Wm, *WT, limit));
              drv.add("mult_B", now_ms() - a); }
            { double a = now_ms();
              result = M::merge(*MA, *MB,
                                [](float x, float y) { return x + y; });
              drv.add("merge", now_ms() - a); }
            if (g_dump_now) dump_matrix(result, "result");
            // CprrSparse.hpp:92 applies updateRankedLists(finalMat) before
            // returning; driver-cprr did not, which made any rk output (and
            // the S2.2b layer-0 spot-check) a no-op copy of the input. Same
            // routine as the LHRR branch, with its own timed ledger entry.
            { double a = now_ms();
              update_ranked_lists(result, rk, N, L);
              drv.add("update_ranked_lists", now_ms() - a); }
            { double a = now_ms();
              MA.reset(); MB.reset(); WT.reset(); Wm.reset();
              drv.add("iter_free", now_ms() - a); }
        } else {
            // LHRRSparse.hpp:35-140 with T=1
            std::unique_ptr<M> MM, H, HT, S, Srev, Stot;
            { double a = now_ms();
              MM = std::make_unique<M>(build_position_from_rk(rk, N, L));
              drv.add("position_build", now_ms() - a); }
            if (g_dump_now) dump_matrix(*MM, "MM");
            { double a = now_ms();
              update_ranked_lists(*MM, rk, N, L);
              drv.add("update_ranked_lists_pos", now_ms() - a); }
            { double a = now_ms();
              H = std::make_unique<M>(build_h_from_rk(rk, N, L, k));
              drv.add("H_build", now_ms() - a); }
            if (g_dump_now) dump_matrix(*H, "H");
            { double a = now_ms();
              HT = std::make_unique<M>(H->transpose());
              drv.add("H_transpose", now_ms() - a); }
            { double a = now_ms();
              S = std::make_unique<M>(M::multiply_and_prune(*H, *HT, limit));
              drv.add("mult_S", now_ms() - a); }
            if (g_dump_now) dump_matrix(*S, "S");
            if (g_validate) validate_wf(*S, "S");
            { double a = now_ms();
              Srev = std::make_unique<M>(M::multiply_and_prune(*HT, *H, limit));
              drv.add("mult_Srev", now_ms() - a); }
            if (g_dump_now) dump_matrix(*Srev, "Srev");
            if (g_validate) validate_wf(*Srev, "Srev");
            { double a = now_ms();
              Stot = std::make_unique<M>(M::combine(*S, *Srev));
              drv.add("combine_S_total", now_ms() - a); }
            if (g_dump_now) dump_matrix(*Stot, "Stot");
            if (g_validate) validate_wf(*Stot, "Stot");
            { double a = now_ms();
              result = M::combine(*MM, *Stot);
              drv.add("combine_main", now_ms() - a); }
            if (g_dump_now) dump_matrix(result, "result");
            if (g_validate) validate_wf(result, "result");
            { double a = now_ms();
              update_ranked_lists(result, rk, N, L);
              drv.add("update_ranked_lists", now_ms() - a); }
            { double a = now_ms();
              S.reset(); Srev.reset(); Stot.reset(); HT.reset(); H.reset();
              MM.reset();
              drv.add("iter_free", now_ms() - a); }
        }
        double ms = now_ms() - t0;
        struct rusage ru_cur;
        getrusage(RUSAGE_SELF, &ru_cur);
        std::printf("REP,%d,%.3f,%zu,%.6f\n", rep, ms, result.nnz(),
                    checksum(result));
        // Mechanism evidence for the rep-0 penalty: every rep allocates and
        // frees the whole pipeline working set, so the first rep of a process
        // pays first-touch page faults that later reps reuse from the allocator.
        std::printf("REPRU,%d,%ld,%ld,%ld\n", rep,
                    ru_cur.ru_minflt - ru_prev.ru_minflt,
                    ru_cur.ru_majflt - ru_prev.ru_majflt,
                    ru_cur.ru_maxrss);
        ru_prev = ru_cur;
        std::fflush(stdout);
#ifdef PHASE_PROFILE
        phprof::report(stdout);
#endif
    }

    drv.report();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef PHASE_PROFILE
    long hwm = phprof::vmhwm_kb();
#else
    long hwm = -1;
#endif
    // PHASE_RK_OUT: raw reranked lists of the LAST rep (each rep restores rk0
    // and recomputes deterministically, so every rep ends with the same rk).
    // Untimed, after the loop; row-major u32 N*L, parsed by rk_cmp.py.
    if (!g_rk_out.empty()) {
        FILE* f = std::fopen(g_rk_out.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "rk_out: cannot open %s\n", g_rk_out.c_str());
        } else {
            std::fwrite(rk.data(), sizeof(uint32_t), rk.size(), f);
            std::fclose(f);
            std::printf("RKOUT,%s,bytes=%zu\n", g_rk_out.c_str(),
                        rk.size() * sizeof(uint32_t));
        }
    }
    std::printf("DONE,%s,%ld,%ld\n", dataset, hwm, ru.ru_maxrss);
    return 0;
}

int main(int argc, char** argv) {
    g_affinity_at_entry = affinity_mask();
    g_validate = (env_or("PHASE_VALIDATE", "0") == std::string("1")) ? 1 : 0;
    // S2.3 step 2: PHASE_BUILDER/PHASE_FRAGAB were removed together with the
    // builders they selected (see the dump_matrix comment block). A stale
    // harness that still exports them gets a loud warning instead of the
    // wrong arm.
    if (const char* b = std::getenv("PHASE_BUILDER"))
        std::fprintf(stderr, "warning: PHASE_BUILDER=%s ignored (S2.3 p2: "
                     "builder is v2 only)\n", b);
    if (const char* b = std::getenv("PHASE_FRAGAB"))
        std::fprintf(stderr, "warning: PHASE_FRAGAB=%s ignored (S2.3 p2: the "
                     "fragment-CSR builder was removed)\n", b);
    {
        std::string rs = env_or("PHASE_ROWSORT", "idx");
        M::set_v2_row_sort_mode(rs == std::string("zip") ? 1 : 0);
    }
    g_dump_dir = env_or("PHASE_DUMP_DIR", "");
    g_rk_out = env_or("PHASE_RK_OUT", "");
    std::printf("ABKNOBS,builder=v2,rowsort=%s,dumpdir=%s,rkout=%s,validate=%d\n",
                M::v2_row_sort_mode() ? "zip" : "idx",
                g_dump_dir.empty() ? "(none)" : g_dump_dir.c_str(),
                g_rk_out.empty() ? "(none)" : g_rk_out.c_str(), g_validate);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <mult|pipe> <w1|w2|w3> [reps=7]\n"
                     "       %s params <w1|w2|w3>\n"
                     "       %s extract <rkLists.txt> <N> <L> <out.bin>\n"
                     "       %s real <bin> <N> <L> <k> <limit> <cprr|lhrr> [reps=7]\n",
                     argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    std::string wl = argv[2];

    // Thread control: OpenMP from PHASE_THREADS/OMP_NUM_THREADS. S2.3 step 2
    // removed the std::execution::par_unseq build sort, so this is the only
    // thread source (no TBB arena is created or configured).
    int omp_threads = omp_get_max_threads();
    if (const char* e = std::getenv("PHASE_THREADS")) {
        int n = std::atoi(e);
        if (n > 0) omp_threads = n;
    }
    omp_set_num_threads(omp_threads);

    bool pp_flag =
#if defined(PHASE_PROFILE)
        true;
#else
        false;
#endif

    if (mode == "extract") {
        if (argc < 6) {
            std::fprintf(stderr, "extract needs <in> <N> <L> <out>\n");
            return 2;
        }
        print_tset(omp_threads);
        size_t N = (size_t)std::strtoull(argv[3], nullptr, 10);
        size_t L = (size_t)std::strtoull(argv[4], nullptr, 10);
        std::vector<uint32_t> rk = extract_rk(argv[2], N, L);
        std::ofstream out(argv[5], std::ios::binary);
        if (!out) { std::fprintf(stderr, "cannot write %s\n", argv[5]); return 2; }
        out.write(reinterpret_cast<const char*>(rk.data()),
                  (std::streamsize)(rk.size() * 4));
        out.close();
        std::printf("DONE,extract,bytes=%zu\n", rk.size() * 4);
        return 0;
    }

    if (mode == "real") {
        if (argc < 9) {
            std::fprintf(stderr,
                         "real needs <bin> <N> <L> <k> <limit> <cprr|lhrr> [reps]\n");
            return 2;
        }
        size_t N = (size_t)std::strtoull(argv[3], nullptr, 10);
        size_t L = (size_t)std::strtoull(argv[4], nullptr, 10);
        int k = std::atoi(argv[5]);
        size_t limit = (size_t)std::strtoull(argv[6], nullptr, 10);
        int reps_real = (argc > 8) ? std::atoi(argv[8]) : 7;
        if (reps_real < 1) reps_real = 1;
        if (std::strcmp(argv[7], "cprr") && std::strcmp(argv[7], "lhrr")) {
            std::fprintf(stderr, "pipeline must be cprr or lhrr\n");
            return 2;
        }
        return run_real(wl.c_str(), argv[2], N, L, k, limit, argv[7], reps_real,
                        omp_threads, (int)pp_flag);
    }

    const Workload* W = nullptr;
    for (const Workload& w : WORKLOADS) if (wl == w.name) W = &w;
    if (!W) { std::fprintf(stderr, "unknown workload %s\n", wl.c_str()); return 2; }

    int reps = (mode == "params" || argc < 4) ? 7 : std::atoi(argv[3]);
    if (reps < 1) reps = 1;
    // Synthetic workloads build A/B/Wsq before the rep loop, so the dump is
    // armed here; the first rep then dumps `result` and disarms it.
    g_dump_now = g_dump_dir.empty() ? 0 : 1;

    std::mt19937 rngA(42), rngB(1000 + (int)W->cols_b);
    M A = random_csr(W->rows_a, W->inner, W->ra, rngA);
    M B = random_csr(W->inner, W->cols_b, W->rb, rngB);
    // pipe mode replicates a CPRR iteration, which needs a SQUARE W (N x N);
    // reuse rows_a as N and ra as k. (A/B above are only for mult mode.)
    M Wsq = (mode == "pipe")
        ? random_csr(W->rows_a, W->rows_a, W->ra, rngA) : M();

    if (mode == "params") {
        print_tset(omp_threads);
        print_params(*W, A, B);
        return 0;
    }

    std::printf("RUN,%s,%s,%d,%d,%d\n", mode.c_str(), W->name, omp_threads, reps,
                (int)pp_flag);
    print_tset(omp_threads);

    M result;
    DrvLedger drv;
    std::unique_ptr<M> prev;   // previous rep's result (freed, and measured)
    for (int rep = 0; rep < reps; ++rep) {
#ifdef PHASE_PROFILE
        phprof::reset();
#endif
        double t0 = now_ms();
        if (mode == "pipe") {
            std::unique_ptr<M> WT, MA, MB;
            { double a = now_ms();
              WT = std::make_unique<M>(Wsq.transpose());
              drv.add("transpose", now_ms() - a); }
            { double a = now_ms();
              MA = std::make_unique<M>(M::multiply_and_prune(*WT, Wsq, W->limit));
              drv.add("mult_A", now_ms() - a); }
            { double a = now_ms();
              MB = std::make_unique<M>(M::multiply_and_prune(Wsq, *WT, W->limit));
              drv.add("mult_B", now_ms() - a); }
            { double a = now_ms();
              result = M::merge(*MA, *MB,
                                [](float x, float y) { return x + y; });
              drv.add("merge", now_ms() - a); }
            // The old driver freed the iteration state at the closing brace of
            // this block, i.e. inside the timed window: make it explicit.
            { double a = now_ms();
              WT.reset(); MA.reset(); MB.reset();
              drv.add("iter_free", now_ms() - a); }
        } else {
            double a = now_ms();
            result = M::multiply_and_prune(A, B, W->limit);
            drv.add("mult_call", now_ms() - a);
            if (g_validate) validate_wf(result, "mult_result");
        }
        double ms = now_ms() - t0;
        // Freeing the previous rep's result used to be inside the timed window
        // (the move-assign destroyed the old buffers): keep it measured.
        if (prev) {
            double a = now_ms();
            prev.reset();
            drv.add("prev_result_dtor", now_ms() - a);
        }
        if (g_dump_now) { dump_matrix(result, "result"); g_dump_now = 0; }
        std::printf("REP,%d,%.3f,%zu,%.6f\n", rep, ms, result.nnz(), checksum(result));
        std::fflush(stdout);
#ifdef PHASE_PROFILE
        phprof::report(stdout);
#endif
        prev = std::make_unique<M>(std::move(result));
        result = M();
    }
    drv.report();

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef PHASE_PROFILE
    long hwm = phprof::vmhwm_kb();
#else
    long hwm = -1;
#endif
    std::printf("DONE,%s,%ld,%ld\n", W->name, hwm, ru.ru_maxrss);
    return 0;
}

