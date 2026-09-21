#ifndef SPARSE_MATRIX_HPP
#define SPARSE_MATRIX_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <numeric>
#include <iostream>
#include <cmath>
#include <omp.h>
#include <execution> 

#include "BlockAllocator.hpp" 

// =========================================================================
// PHASE PROFILE (Phase 0 instrumentation) -- compile-time opt-in ONLY.
// Build with -DPHASE_PROFILE to enable. Without the flag every line below
// compiles out: no extra branches, no extra memory, no API changes.
//
//   * Per-thread counters live in 128B-aligned slots (same pattern as
//     PaddedAllocator) so adjacent threads never share a cache line.
//   * Ticks come from rdtsc (x86-64) with a steady_clock fallback.
//   * f1 (accumulate) / f2 (prune+emit) alternate per row, so their ticks
//     are accumulated in stack-local RowTimers and merged once per
//     parallel region. PHASE_PROFILE_ROW_SAMPLE (default 1) times 1-in-N
//     rows if rdtsc overhead exceeds ~3%.
//   * Build sub-phases: B0 temp_triplets_ resize (serial), B1 copy,
//     B2 sort, B3 histogram+reduction+prefix, B4 values_/col_idx_ resize
//     (serial), B5 fill. Serial phases record wall ticks + getrusage
//     deltas (user/sys time, page faults); parallel build regions record
//     an aggregated rusage delta around the region. The multiply/merge/
//     combine row loops record ticks + per-thread end stamps only.
//   * Skew: parallel regions stamp each thread's end time; report()
//     prints max(end) - mean(end) per phase.
//   * Memory: VmHWM (/proc/self/status) sampled at build entry/exit and
//     at report time.
//   * report() emits machine-parseable CSV lines prefixed PHASEPROFILE.
// =========================================================================
#ifdef PHASE_PROFILE
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

#ifndef PHASE_PROFILE_ROW_SAMPLE
#define PHASE_PROFILE_ROW_SAMPLE 1
#endif

namespace phprof {

#if defined(__x86_64__) || defined(__i386__)
#define PHASE_PROFILE_RDTSC 1
inline uint64_t tick() { return __builtin_ia32_rdtsc(); }
#else
#define PHASE_PROFILE_RDTSC 0
inline uint64_t tick() {
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}
#endif

// Phase ids. Per-thread/parallel phases first, then serial wall phases.
//
// Etapa A (A0) adds the call-level phases and the remaining build sub-phases
// so that REP = sum(call-level walls) + residue, with no uninstrumented
// phase left in the CPRR/LHRR pipelines (mult/pipe ledger).
enum : int {
    P_F1_ACCUM = 0,    // multiply: per-row accumulate (timed rows only)
    P_F2_PRUNE,        // multiply: per-row nth_element prune + emit
    P_MERGE_LOOP,      // merge(): per-thread row loop
    P_COMBINE_LOOP,    // combine(): per-thread row loop
    P_B1_COPY,         // build: parallel triplet copy
    P_B3_HIST,         // build: histogram (parallel region only)
    P_B5_FILL,         // build: parallel fill of values_/col_idx_
    P_B0_TEMP_RESIZE,  // build: serial resize of temp_triplets_
    P_B2_SORT,         // build: global sort (wall; TBB threads inside)
    P_B4_VALS_RESIZE,  // build: serial resize of values_/col_idx_
    P_TRANSPOSE,       // transpose() whole (serial)
    P_MULT_TOTAL,      // multiply_and_prune: per-thread whole-region wall
    // ---- A0: sub-phases (per thread / region) ----
    P_ACC_PREP,        // mult: bound scan + accumulator ctor/reserve
    P_B3_SETUP,        // build: thread_row_counts + row_counts alloc (serial)
    P_B3_RED,          // build: parallel reduction of thread histograms
    P_B3_PREFIX,       // build: serial prefix sum into row_ptr_
    P_BUILD_FINISH,    // build: release of the shared sorted triplet buffer
    // ---- A0: call-level walls (serial, whole function) ----
    P_ALLOC_SETUP,     // call-level: PaddedAllocator vector ctor + ranges
    P_ALLOC_TEARDOWN,  // call-level: PaddedAllocator dtor (2 MB block frees)
    P_MP_CALL,         // call-level: whole multiply_and_prune
    P_MERGE_CALL,      // call-level: whole merge
    P_COMBINE_CALL,    // call-level: whole combine
    P_BUILD_CALL,      // call-level: whole build_from_partial_results
    P_MAX
};
constexpr int MAX_THREADS = 128;

struct alignas(128) ThreadCtr {
    uint64_t ticks[P_MAX]; // accumulated ticks per phase (this thread)
    uint64_t start[P_MAX]; // region-entry stamp (skew)
    uint64_t end[P_MAX];   // region-exit stamp (skew)
    uint64_t calls[P_MAX]; // phase invocations by this thread
    uint64_t rows_total;   // rows processed by the multiply loop
    uint64_t rows_f1;      // rows actually timed for f1/f2 (sampling)
    uint64_t emitted;      // triplets emitted by this thread
    uint64_t bound_sum;    // sum of per-row fan-out bounds
    // A0: accumulator branch actually taken by this thread + worst bound.
    uint64_t hash_threads;   // 1 if this thread used the hash accumulator
    uint64_t dense_threads;  // 1 if this thread used the dense accumulator
    uint64_t max_bound;      // max per-row bound seen in this thread's chunk
    void clear() { std::memset(this, 0, sizeof(*this)); }
};

inline ThreadCtr* counters() {
    static std::vector<ThreadCtr> v(MAX_THREADS); // value-initialized (zeroed)
    return v.data();
}

struct RUsageAcc { long user_us = 0, sys_us = 0, minflt = 0, majflt = 0, n = 0; };
struct RUsageSnap { long u, s, mf, Mf; };

inline RUsageSnap rsnap() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    RUsageSnap s;
    s.u  = ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
    s.s  = ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec;
    s.mf = ru.ru_minflt;
    s.Mf = ru.ru_majflt;
    return s;
}

inline RUsageAcc& rusage_acc(int phase) {
    static RUsageAcc a[P_MAX];
    return a[phase];
}

inline long vmhwm_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "VmHWM: %ld kB", &kb) == 1) break;
    std::fclose(f);
    return kb;
}

struct Globals {
    double ticks_per_ms = 0.0;
    long hwm_before_build_max = -1;
    long hwm_after_build_max = -1;
    long hwm_final = -1;
    uint64_t build_calls = 0;
    // A0: memory decomposition support (PHASE_MEM=1 enables sampling).
    int mem_mode = -1;              // lazily read from getenv("PHASE_MEM")
    long hwm_reset_last_rc = -1;    // result of the last /proc/self/clear_refs write
    uint64_t mem_builds_sampled = 0;
    // A0: last multiply's B.cols_, so the driver can print the dense/hash ratio
    // next to the measured max bound (Phase 4 calibration input).
    uint64_t last_mult_cols_b = 0;
};
inline Globals& g() { static Globals x; return x; }

// ---------------------------------------------------------------------------
// A0: memory sampling helpers (opt-in at runtime via PHASE_MEM=1, so the
// timing runs are never perturbed by barriers/proc reads).
// ---------------------------------------------------------------------------
inline bool mem_enabled() {
    Globals& G = g();
    if (G.mem_mode < 0) {
        const char* e = std::getenv("PHASE_MEM");
        G.mem_mode = (e && e[0] != '0') ? 1 : 0;
    }
    return G.mem_mode == 1;
}

inline long vmrss_kb() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    std::fclose(f);
    return kb;
}

// Rewind the kernel's peak-RSS high-water mark to the *current* RSS by
// writing 5 to /proc/self/clear_refs (Linux >= 4.0; own process only).
inline long hwm_reset() {
    FILE* f = std::fopen("/proc/self/clear_refs", "w");
    if (!f) return -1;
    int rc = std::fputs("5\n", f);
    int cerr = std::fclose(f);
    long ok = (rc >= 0 && cerr == 0) ? 1 : 0;
    g().hwm_reset_last_rc = ok;
    return ok;
}

inline void mem_point(const char* tag) {
    if (!mem_enabled()) return;
    std::printf("PHASEPROFILE,MEM,%s,rss_kb=%ld,hwm_kb=%ld\n",
                tag, vmrss_kb(), vmhwm_kb());
    std::fflush(stdout);
}

// Call from INSIDE a parallel region only (every thread must reach it);
// thread 0 samples while the others wait, so the snapshot is well defined.
inline void mem_point_sync(const char* tag) {
    if (!mem_enabled()) return;
    if (omp_get_thread_num() == 0) mem_point(tag);
#pragma omp barrier
}

// Same, but safe in *divergent* code paths (no barrier): thread 0 samples at
// its own progress point. Use only where a barrier would be illegal.
inline void mem_point_t0(const char* tag) {
    if (!mem_enabled()) return;
    if (omp_get_thread_num() == 0) mem_point(tag);
}

// Add ticks to the calling thread's slot without a region-level stamp
// (used for sub-phases inside a parallel region, e.g. ACC_PREP).
inline void accum_ticks(int tid, int phase, uint64_t dt) {
    counters()[tid].ticks[phase] += dt;
    ++counters()[tid].calls[phase];
}

// Per-build ledger: emitted triplets, the buffers involved and the HWM delta
// measured after a clear_refs reset taken before the build (A3 requirement).
inline void mem_build(size_t emitted, size_t tmp_bytes, size_t out_bytes,
                      size_t alloc_bytes, size_t count_bytes,
                      long hwm_before, long hwm_after) {
    if (!mem_enabled()) return;
    ++g().mem_builds_sampled;
    double per_triplet = emitted ? (double)(hwm_after - hwm_before) * 1024.0 /
                                       (double)emitted : 0.0;
    std::printf(
        "PHASEPROFILE,MEMBUILD,n=%llu,emitted=%zu,tmp_bytes=%zu,out_bytes=%zu,"
        "alloc_bytes=%zu,count_bytes=%zu,hwm_before_kb=%ld,hwm_after_kb=%ld,"
        "delta_hwm_kb=%ld,bytes_per_triplet=%.3f,reset_rc=%ld\n",
        (unsigned long long)g().mem_builds_sampled, emitted, tmp_bytes,
        out_bytes, alloc_bytes, count_bytes, hwm_before, hwm_after,
        hwm_after - hwm_before, per_triplet, g().hwm_reset_last_rc);
    std::fflush(stdout);
}

// Serial phase (wall ticks + rusage deltas), mid-function friendly.
inline void serial_begin(int /*phase*/, uint64_t& t0, RUsageSnap& r0) {
    t0 = tick();
    r0 = rsnap();
}
inline void serial_end(int phase, uint64_t t0, RUsageSnap r0) {
    ThreadCtr& c = counters()[omp_get_thread_num()];
    c.ticks[phase] += tick() - t0;
    ++c.calls[phase];
    RUsageSnap r1 = rsnap();
    RUsageAcc& a = rusage_acc(phase);
    a.user_us += r1.u - r0.u;
    a.sys_us += r1.s - r0.s;
    a.minflt += r1.mf - r0.mf;
    a.majflt += r1.Mf - r0.Mf;
    ++a.n;
}

// Rusage-only wrappers for parallel regions (called from serial context;
// Linux getrusage(RUSAGE_SELF) is process-wide, so this must NOT be done
// per thread).
inline void ru_begin(int /*phase*/, RUsageSnap& r0) { r0 = rsnap(); }
inline void ru_end(int phase, RUsageSnap r0) {
    RUsageSnap r1 = rsnap();
    RUsageAcc& a = rusage_acc(phase);
    a.user_us += r1.u - r0.u;
    a.sys_us += r1.s - r0.s;
    a.minflt += r1.mf - r0.mf;
    a.majflt += r1.Mf - r0.Mf;
    ++a.n;
}

// Parallel phase: one PhaseMark per thread inside the region; the
// destructor stamps the region exit for the skew metric.
struct PhaseMark {
    int tid, phase;
    uint64_t t0;
    explicit PhaseMark(int p) : phase(p) {
        tid = omp_get_thread_num();
        t0 = tick();
        counters()[tid].start[phase] = t0;
    }
    ~PhaseMark() {
        uint64_t t1 = tick();
        ThreadCtr& c = counters()[tid];
        c.ticks[phase] += t1 - t0;
        c.end[phase] = t1;
        ++c.calls[phase];
    }
};

// Stack-local per-row timers for f1/f2; merged once per parallel region
// into the owning thread's aligned slot.
struct RowTimer {
    uint64_t f1 = 0, f2 = 0, rows = 0, emitted = 0;
};

inline void reset() {
    for (int i = 0; i < MAX_THREADS; ++i) counters()[i].clear();
    for (int p = 0; p < P_MAX; ++p) rusage_acc(p) = RUsageAcc{};
    Globals& G = g();
    G.hwm_before_build_max = -1;
    G.hwm_after_build_max = -1;
}

inline void report(FILE* out) {
    Globals& G = g();
#if PHASE_PROFILE_RDTSC
    if (G.ticks_per_ms <= 0.0) {
        auto t0c = std::chrono::steady_clock::now();
        uint64_t c0 = tick();
        for (;;) {
            auto t1c = std::chrono::steady_clock::now();
            if (std::chrono::duration<double, std::milli>(t1c - t0c).count() >= 20.0)
                break;
        }
        uint64_t c1 = tick();
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0c).count();
        G.ticks_per_ms = (double)(c1 - c0) / (ms > 1.0 ? ms : 1.0);
    }
#else
    if (G.ticks_per_ms <= 0.0) G.ticks_per_ms = 1.0e6; // steady_clock ns ticks
#endif
    G.hwm_final = vmhwm_kb();
    std::fprintf(out,
        "PHASEPROFILE,HWM,before_build_max_kb=%ld,after_build_max_kb=%ld,"
        "final_kb=%ld,rss_final_kb=%ld,build_calls=%llu\n",
        G.hwm_before_build_max, G.hwm_after_build_max, G.hwm_final, vmrss_kb(),
        (unsigned long long)G.build_calls);

    static const char* names[P_MAX] = {
        "F1_ACCUM", "F2_PRUNE", "MERGE_LOOP", "COMBINE_LOOP",
        "B1_COPY", "B3_HIST", "B5_FILL",
        "B0_TEMP_RESIZE", "B2_SORT", "B4_VALS_RESIZE", "TRANSPOSE",
        "MULT_TOTAL",
        "ACC_PREP", "B3_SETUP", "B3_RED", "B3_PREFIX", "BUILD_FINISH",
        "ALLOC_SETUP", "ALLOC_TEARDOWN", "MP_CALL", "MERGE_CALL",
        "COMBINE_CALL", "BUILD_CALL"
    };
    const ThreadCtr* C = counters();
    for (int p = 0; p < P_MAX; ++p) {
        uint64_t total = 0, calls = 0, maxend = 0, endsum = 0, minstart = 0;
        int nend = 0;
        uint64_t ctx1 = 0, ctx2 = 0;
        bool has_stamp = false;
        for (int t = 0; t < MAX_THREADS; ++t) {
            total += C[t].ticks[p];
            calls += C[t].calls[p];
            if (C[t].end[p] > 0) {
                has_stamp = true;
                if (C[t].end[p] > maxend) maxend = C[t].end[p];
                endsum += C[t].end[p];
                ++nend;
                if (minstart == 0 ||
                    (C[t].start[p] > 0 && C[t].start[p] < minstart))
                    minstart = C[t].start[p];
            }
            switch (p) {
                case P_F1_ACCUM: case P_F2_PRUNE:
                    ctx1 += C[t].rows_f1; ctx2 += C[t].rows_total; break;
                case P_MULT_TOTAL: case P_MERGE_LOOP: case P_COMBINE_LOOP:
                    ctx1 += C[t].rows_total; ctx2 += C[t].emitted; break;
                default: break;
            }
        }
        double total_ms = (double)total / G.ticks_per_ms;
        double wall_ms = (has_stamp && minstart > 0)
                             ? (double)(maxend - minstart) / G.ticks_per_ms
                             : total_ms; // serial/no-stamp phases
        double skew_ms = (has_stamp && nend > 0)
                             ? (double)(maxend - endsum / (uint64_t)nend) /
                                   G.ticks_per_ms
                             : 0.0;
        RUsageAcc& a = rusage_acc(p);
        std::fprintf(out,
            "PHASEPROFILE,%s,total_ms=%.3f,wall_ms=%.3f,skew_ms=%.3f,"
            "user_ms=%.3f,sys_ms=%.3f,minflt=%ld,majflt=%ld,calls=%llu,"
            "ctx1=%llu,ctx2=%llu\n",
            names[p], total_ms, wall_ms, skew_ms,
            (double)a.user_us / 1000.0, (double)a.sys_us / 1000.0,
            a.minflt, a.majflt, (unsigned long long)calls,
            (unsigned long long)ctx1, (unsigned long long)ctx2);
    }

    // -----------------------------------------------------------------------
    // A0: per-thread breakdown (skew source, allocator/emission decomposition)
    // and the dense/hash decision actually taken, with the measured bound.
    // -----------------------------------------------------------------------
    uint64_t rows_all = 0, emitted_all = 0, bound_all = 0, maxb = 0;
    uint64_t nh = 0, nd = 0;
    for (int t = 0; t < MAX_THREADS; ++t) {
        const ThreadCtr& c = C[t];
        if (c.rows_total == 0 && c.emitted == 0 && c.bound_sum == 0) continue;
        rows_all += c.rows_total; emitted_all += c.emitted;
        bound_all += c.bound_sum;
        if (c.max_bound > maxb) maxb = c.max_bound;
        nh += c.hash_threads; nd += c.dense_threads;
        std::fprintf(out,
            "PHASEPROFILE,THREAD,tid=%d,rows=%llu,emitted=%llu,bound_sum=%llu,"
            "max_bound=%llu,hash=%llu,dense=%llu,mult_ms=%.3f\n",
            t, (unsigned long long)c.rows_total,
            (unsigned long long)c.emitted, (unsigned long long)c.bound_sum,
            (unsigned long long)c.max_bound, (unsigned long long)c.hash_threads,
            (unsigned long long)c.dense_threads,
            (double)c.ticks[P_MULT_TOTAL] / G.ticks_per_ms);
    }
    std::fprintf(out,
        "PHASEPROFILE,CTX,rows=%llu,emitted=%llu,bound_sum=%llu,max_bound=%llu,"
        "hash_threads=%llu,dense_threads=%llu,cols_b=%llu,ratio=%.3f,branch=%s\n",
        (unsigned long long)rows_all, (unsigned long long)emitted_all,
        (unsigned long long)bound_all, (unsigned long long)maxb,
        (unsigned long long)nh, (unsigned long long)nd,
        (unsigned long long)G.last_mult_cols_b,
        maxb ? (double)G.last_mult_cols_b / (double)maxb : 0.0,
        nh ? "hash" : (nd ? "dense" : "n/a"));
}

} // namespace phprof
#endif // PHASE_PROFILE

template <typename T>
class CSRMatrix {
public:
    using value_type = T;
    using size_type = uint32_t;
    using index_type = uint32_t;

    struct Triplet {
        size_type row;
        size_type col;
        T value;
        bool is_set; 
        
        Triplet() : row(0), col(0), value(T{}), is_set(false) {}
        Triplet(size_type r, size_type c, T v, bool set = false)
            : row(r), col(c), value(v), is_set(set) {}
    };

    // CRITICAL FIX: Heap-allocated padded structure to prevent false sharing
    struct PaddedAllocator {
        alignas(128) BlockAllocator<Triplet>* ptr;  // 128-byte alignment for safety
        char padding[128 - sizeof(BlockAllocator<Triplet>*)];  // Explicit padding
        
        PaddedAllocator() : ptr(nullptr) {
            // Allocate on heap with alignment
            ptr = new (std::align_val_t(128)) BlockAllocator<Triplet>();
        }
        
        ~PaddedAllocator() {
            if (ptr) {
                ptr->~BlockAllocator();
                ::operator delete(ptr, std::align_val_t(128));
            }
        }
        
        // Delete copy/move
        PaddedAllocator(const PaddedAllocator&) = delete;
        PaddedAllocator& operator=(const PaddedAllocator&) = delete;
        PaddedAllocator(PaddedAllocator&& other) noexcept : ptr(other.ptr) {
            other.ptr = nullptr;
        }
        PaddedAllocator& operator=(PaddedAllocator&& other) noexcept {
            if (this != &other) {
                if (ptr) {
                    ptr->~BlockAllocator();
                    ::operator delete(ptr, std::align_val_t(128));
                }
                ptr = other.ptr;
                other.ptr = nullptr;
            }
            return *this;
        }
    };

    CSRMatrix() : rows_(0), cols_(0), finalized_(false) {}
    CSRMatrix(size_type rows, size_type cols, size_t estimated_nnz = 0)
        : rows_(rows), cols_(cols), finalized_(false) {
        row_ptr_.resize(rows + 1, 0);
        if (estimated_nnz > 0) {
            values_.reserve(estimated_nnz);
            col_idx_.reserve(estimated_nnz);
        }
    }

    CSRMatrix(CSRMatrix&& other) noexcept = default;
    CSRMatrix& operator=(CSRMatrix&& other) noexcept = default;

    // =========================================================================
    // OPTIMIZED "ROW-SPLIT" ENGINE
    // =========================================================================

    // =========================================================================
    // FASE 1: Fragmento-CSR por thread (build_from_ordered_fragments)
    //
    // Assumes each thread's PaddedAllocator emits Triplets ordered by
    // (row, col) within its assigned row range. Eliminates the global
    // temp_triplets_ buffer, the O(M log M) parallel sort (B2), and the
    // parallel histogram/reduce (B3). Instead performs a direct sequential
    // write into the final CSR arrays.
    //
    // Caller invariant:
    //   - thread tid emits rows strictly within row_ranges[tid]
    //   - within those rows, Triplets are sorted by (row, col) with
    //     duplicates coalesced (is_set=true replaces, is_set=false sums)
    //
    // For the symmetric mainMatrix build in LHRRSparse (which emits
    // t2->row = neighbor, i.e. rows outside the thread's range), callers
    // MUST use build_from_partial_results instead.
    // =========================================================================
    static void build_from_ordered_fragments(
        CSRMatrix& C,
        std::vector<PaddedAllocator>& allocators,
        const std::vector<std::pair<size_type, size_type>>& row_ranges
    ) {
        if (C.finalized_) throw std::logic_error("Matrix finalized");

#ifdef PHASE_PROFILE
        uint64_t t0_bcall = 0; phprof::RUsageSnap r0_bcall;
        size_t alloc_bytes = 0;
        long hwm_b0 = -1;
        phprof::serial_begin(phprof::P_BUILD_CALL, t0_bcall, r0_bcall);
        for (int i = 0; i < (int)allocators.size(); ++i)
            alloc_bytes += allocators[i].ptr->capacity_bytes();
        if (phprof::mem_enabled()) {
            phprof::hwm_reset();
            hwm_b0 = phprof::vmhwm_kb();
            phprof::mem_point("buildFrag_entry");
        }
#endif

        int num_threads = allocators.size();

        // 1. Compute per-thread sizes and total
        std::vector<size_t> thread_sizes(num_threads);
        size_t total_elements = 0;
        for (int t = 0; t < num_threads; ++t) {
            thread_sizes[t] = allocators[t].ptr->size();
            total_elements += thread_sizes[t];
        }

        std::cout << "  [BuildFrag] Total triplets: " << total_elements << std::endl;

        // 2. Count rows via single pass per thread (no global sort needed).
        //    Invariant: in multiply_and_prune/merge/combine, each thread
        //    emits rows strictly within its range with unique (row, col)
        //    within a row. No cross-thread duplicates.
        std::vector<size_type> row_counts(C.rows_, 0);

#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_b3 = phprof::rsnap();
#endif
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            if (tid < num_threads && thread_sizes[tid] > 0) {
                allocators[tid].ptr->for_each([&](const Triplet& t) {
                    row_counts[t.row]++;
                });
            }
        }
#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B3_HIST, r0_b3);
        phprof::mem_point("after_B3_frag_count");
#endif

        // 3. Prefix sum -> row_ptr_
#ifdef PHASE_PROFILE
        uint64_t t0_b3p; phprof::RUsageSnap r0_b3p;
        phprof::serial_begin(phprof::P_B3_PREFIX, t0_b3p, r0_b3p);
#endif
        C.row_ptr_[0] = 0;
        for (size_t i = 0; i < C.rows_; ++i)
            C.row_ptr_[i+1] = C.row_ptr_[i] + row_counts[i];
        size_t final_nnz = C.row_ptr_[C.rows_];

#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B3_PREFIX, t0_b3p, r0_b3p);
#endif

        // 4. Allocate output arrays (FINAL size -- no temp_triplets_ buffer!)
#ifdef PHASE_PROFILE
        uint64_t t0_b4; phprof::RUsageSnap r0_b4;
        phprof::serial_begin(phprof::P_B4_VALS_RESIZE, t0_b4, r0_b4);
#endif
        C.values_.resize(final_nnz);
        C.col_idx_.resize(final_nnz);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B4_VALS_RESIZE, t0_b4, r0_b4);
        phprof::mem_point("after_B4_frag_resize");
#endif

        // 5. Parallel fill: each thread writes its rows directly into output.
        //    Rows are disjoint per thread and triplet stream is (row, col)
        //    sorted, so a running write cursor per thread works.
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_fill = phprof::rsnap();
#endif
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            if (tid < num_threads && thread_sizes[tid] > 0) {
                size_t r_start = row_ranges[tid].first;
                size_t write_pos = C.row_ptr_[r_start];

                allocators[tid].ptr->for_each([&](const Triplet& t) {
                    C.values_[write_pos] = t.value;
                    C.col_idx_[write_pos] = t.col;
                    write_pos++;
                });
            }
        }
#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B5_FILL, r0_fill);
        phprof::mem_point("after_B5_frag_fill");
#endif

        C.finalized_ = true;

#ifdef PHASE_PROFILE
        {
            long hwm_a = phprof::vmhwm_kb();
            if (hwm_a > phprof::g().hwm_after_build_max)
                phprof::g().hwm_after_build_max = hwm_a;
            size_t out_bytes = C.values_.size() * sizeof(T) +
                               C.col_idx_.size() * sizeof(index_type);
            phprof::mem_build(total_elements, 0, out_bytes, alloc_bytes,
                              0, hwm_b0, hwm_a);
            phprof::mem_point("buildFrag_exit");
            phprof::serial_end(phprof::P_BUILD_CALL, t0_bcall, r0_bcall);
        }
#endif
    }


    static void build_from_partial_results(
        CSRMatrix& C, 
        std::vector<PaddedAllocator>& allocators,
        const std::vector<std::pair<size_type, size_type>>& row_ranges
    ) {
        if (C.finalized_) throw std::logic_error("Matrix finalized");
#ifdef PHASE_PROFILE
        // A0: call-level wall for the whole build + the per-build memory
        // ledger (allocator bytes, tmp/out/count buffers, HWM delta after a
        // clear_refs reset taken at entry).
        uint64_t t0_bcall = 0; phprof::RUsageSnap r0_bcall;
        size_t alloc_bytes = 0;
        long hwm_b0 = -1;
        phprof::serial_begin(phprof::P_BUILD_CALL, t0_bcall, r0_bcall);
        for (int i = 0; i < (int)allocators.size(); ++i)
            alloc_bytes += allocators[i].ptr->capacity_bytes();
        {   long h = phprof::vmhwm_kb();
            if (h > phprof::g().hwm_before_build_max)
                phprof::g().hwm_before_build_max = h;
            ++phprof::g().build_calls;
        }
        if (phprof::mem_enabled()) {
            phprof::hwm_reset();               // peak rewind: measure THIS build
            hwm_b0 = phprof::vmhwm_kb();
            phprof::mem_point("build_entry");
        }
#endif

        int num_threads = allocators.size();

        // 1. Calculate offsets (Sequential - very fast)
        std::vector<size_t> thread_offsets(num_threads + 1, 0);
        for (int i = 0; i < num_threads; ++i) {
            thread_offsets[i+1] = thread_offsets[i] + allocators[i].ptr->size();
        }
        size_t total_elements = thread_offsets.back();

        std::cout << "  [Build] Total triplets: " << total_elements << std::endl;

#ifdef PHASE_PROFILE
        uint64_t t0_b0; phprof::RUsageSnap r0_b0;
        phprof::serial_begin(phprof::P_B0_TEMP_RESIZE, t0_b0, r0_b0);
#endif
        // 2. Allocate destination buffer
        C.temp_triplets_.resize(total_elements);

#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B0_TEMP_RESIZE, t0_b0, r0_b0);
        phprof::mem_point("after_B0_tmp_resize");
#endif
        // 3. OPTIMIZED: Parallel copy with prefetching
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_b1 = phprof::rsnap();
#endif
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            
#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_b1(phprof::P_B1_COPY);
#endif
            if (tid < num_threads && allocators[tid].ptr->size() > 0) {
                auto dest = C.temp_triplets_.begin() + thread_offsets[tid];
                
                // Prefetch hint for next thread's data
                if (tid + 1 < num_threads) {
                    __builtin_prefetch(allocators[tid + 1].ptr, 0, 3);
                }
                
                allocators[tid].ptr->copy_to(dest);
            }
        }

#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B1_COPY, r0_b1);
#endif
        // 4. Parallel sort (Already optimal)
#ifdef PHASE_PROFILE
        uint64_t t0_b2; phprof::RUsageSnap r0_b2;
        phprof::serial_begin(phprof::P_B2_SORT, t0_b2, r0_b2);
#endif
        std::sort(std::execution::par_unseq, 
                  C.temp_triplets_.begin(), C.temp_triplets_.end(),
                  [](const Triplet& a, const Triplet& b) {
                      if (a.row != b.row) return a.row < b.row;
                      return a.col < b.col;
                  });

#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B2_SORT, t0_b2, r0_b2);
        phprof::mem_point("after_B2_sort");
#endif
        // 5. OPTIMIZED CSR Construction with NUMA awareness
        buildCSRParallel(C, total_elements, row_ranges, num_threads);
        
#ifdef PHASE_PROFILE
        {   long h = phprof::vmhwm_kb();
            if (h > phprof::g().hwm_after_build_max)
                phprof::g().hwm_after_build_max = h;
        }
#endif
        C.finalized_ = true;
#ifdef PHASE_PROFILE
        uint64_t t0_bfin; phprof::RUsageSnap r0_bfin;
        phprof::serial_begin(phprof::P_BUILD_FINISH, t0_bfin, r0_bfin);
#endif
        std::vector<Triplet>().swap(C.temp_triplets_);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_BUILD_FINISH, t0_bfin, r0_bfin);
        {
            // A0/A3 ledger: what this build allocated vs what it produced,
            // with the HWM delta measured from the reset taken at entry.
            size_t out_bytes = C.values_.size() * sizeof(T) +
                               C.col_idx_.size() * sizeof(index_type);
            size_t tmp_bytes = total_elements * sizeof(Triplet);
            size_t count_bytes =
                (size_t)num_threads * C.rows_ * sizeof(size_type) +
                C.rows_ * sizeof(size_type);
            long hwm_a = phprof::vmhwm_kb();
            phprof::mem_point("build_exit");
            phprof::mem_build(total_elements, tmp_bytes, out_bytes, alloc_bytes,
                              count_bytes, hwm_b0, hwm_a);
            phprof::serial_end(phprof::P_BUILD_CALL, t0_bcall, r0_bcall);
        }
#endif
    }

private:
    static void buildCSRParallel(CSRMatrix& C, size_t total_elements, 
                                 const std::vector<std::pair<size_type, size_type>>& row_ranges,
                                 int num_threads) {
        
        // A. NUMA-aware histogram (each thread on its own memory)
        std::vector<std::vector<size_type>> thread_row_counts;
#ifdef PHASE_PROFILE
        uint64_t t0_bset; phprof::RUsageSnap r0_bset;
        phprof::serial_begin(phprof::P_B3_SETUP, t0_bset, r0_bset);
#endif
        thread_row_counts.resize(num_threads);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B3_SETUP, t0_bset, r0_bset);
#endif
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_hist = phprof::rsnap();
#endif
        
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_hist(phprof::P_B3_HIST);
#endif
            thread_row_counts[tid].resize(C.rows_, 0);
            
            size_t chunk = (total_elements + num_threads - 1) / num_threads;
            size_t start = std::min(total_elements, (size_t)tid * chunk);
            size_t end   = std::min(total_elements, (size_t)(tid + 1) * chunk);
            
            // Prefetch hint
            if (start < end) {
                __builtin_prefetch(&C.temp_triplets_[start], 0, 3);
            }
            
            for (size_t i = start; i < end; ++i) {
                // Skip duplicates
                if (i > 0 && 
                    C.temp_triplets_[i].row == C.temp_triplets_[i-1].row && 
                    C.temp_triplets_[i].col == C.temp_triplets_[i-1].col) {
                    continue;
                }
                thread_row_counts[tid][C.temp_triplets_[i].row]++;
            }
        }

#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B3_HIST, r0_hist);
        phprof::mem_point("after_B3_hist");
#endif
        // B. Parallel reduction
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_red = phprof::rsnap();
#endif
        std::vector<size_type> row_counts(C.rows_, 0);
        
        #pragma omp parallel for
        for (size_t r = 0; r < C.rows_; ++r) {
#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_red(phprof::P_B3_RED);
#endif
            size_type sum = 0;
            for (int t = 0; t < num_threads; ++t) {
                sum += thread_row_counts[t][r];
            }
            row_counts[r] = sum;
        }

#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B3_RED, r0_red);
#endif
        // C. Prefix sum (Sequential but fast)
#ifdef PHASE_PROFILE
        uint64_t t0_pre; phprof::RUsageSnap r0_pre;
        phprof::serial_begin(phprof::P_B3_PREFIX, t0_pre, r0_pre);
#endif
        C.row_ptr_[0] = 0;
        for (size_t i = 0; i < C.rows_; ++i) {
            C.row_ptr_[i+1] = C.row_ptr_[i] + row_counts[i];
        }
        
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B3_PREFIX, t0_pre, r0_pre);
#endif
        size_t final_nnz = C.row_ptr_[C.rows_];
#ifdef PHASE_PROFILE
        uint64_t t0_b4; phprof::RUsageSnap r0_b4;
        phprof::serial_begin(phprof::P_B4_VALS_RESIZE, t0_b4, r0_b4);
#endif
        C.values_.resize(final_nnz);
        C.col_idx_.resize(final_nnz);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B4_VALS_RESIZE, t0_b4, r0_b4);
        phprof::mem_point("after_B4_out_resize");
#endif

        // D. Parallel fill with cache-aware iteration
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_fill = phprof::rsnap();
#endif
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            
#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_fill(phprof::P_B5_FILL);
#endif
            if (tid < (int)row_ranges.size()) {
                size_t r_start = row_ranges[tid].first;
                size_t r_end   = row_ranges[tid].second;

                // Binary search for range
                auto it_start = std::lower_bound(
                    C.temp_triplets_.begin(), C.temp_triplets_.end(), r_start,
                    [](const Triplet& t, size_t val) { return t.row < val; }
                );
                
                auto it_end = std::lower_bound(
                    C.temp_triplets_.begin(), C.temp_triplets_.end(), r_end,
                    [](const Triplet& t, size_t val) { return t.row < val; }
                );

                size_t vec_start = std::distance(C.temp_triplets_.begin(), it_start);
                size_t vec_end   = std::distance(C.temp_triplets_.begin(), it_end);
                size_t write_pos = C.row_ptr_[r_start];

                // Prefetch destination
                if (write_pos < final_nnz) {
                    __builtin_prefetch(&C.values_[write_pos], 1, 3);
                    __builtin_prefetch(&C.col_idx_[write_pos], 1, 3);
                }
                
                for (size_t i = vec_start; i < vec_end; ++i) {
                    if (i > vec_start && 
                        C.temp_triplets_[i].row == C.temp_triplets_[i-1].row &&
                        C.temp_triplets_[i].col == C.temp_triplets_[i-1].col) 
                    {
                        // Duplicate: accumulate or set
                        if (C.temp_triplets_[i].is_set) {
                            C.values_[write_pos-1] = C.temp_triplets_[i].value;
                        } else {
                            C.values_[write_pos-1] += C.temp_triplets_[i].value;
                        }
                    } else {
                        // New entry
                        C.values_[write_pos] = C.temp_triplets_[i].value;
                        C.col_idx_[write_pos] = C.temp_triplets_[i].col;
                        write_pos++;
                    }
                }
            }
        }
#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B5_FILL, r0_fill);
        phprof::mem_point("after_B5_fill");
#endif
    }
private:
    // =========================================================================
    // Row accumulators for multiply_and_prune
    //
    // The previous implementation used one O(B.cols_) dense array per thread
    // (`dense_acc`/`marker`). Real touched work per row is bounded by
    // bound_i = sum_{k in row_i(A)} nnz_B(k)  -- i.e. O(k^2), not O(n) --
    // but that O(n) buffer means the k^2 touches are scattered across a
    // huge, mostly-empty address range, which is the actual source of the
    // cache/TLB pressure (not extra arithmetic work: the inner double loop
    // was already bounded by k^2).
    //
    // HashRowAccumulator below caps the accumulator's footprint at
    // O(max_bound) for the thread's row range, so it can stay cache
    // resident regardless of B.cols_. Empirically (see
    // Source/bench/bench_accumulator.cpp) it wins once B.cols_ is roughly
    // ADAPTIVE_RATIO times the bound; below that the extra hashing/probing
    // cost isn't worth it and the plain dense array remains faster, so
    // multiply_and_prune picks per-thread-chunk between the two.
    // =========================================================================

    struct DenseRowAccumulator {
        std::vector<T> val;
        std::vector<index_type> marker;
        std::vector<size_t> active_;
        index_type current_row_ = (index_type)-1;

        explicit DenseRowAccumulator(size_type n) : val(n, T{}), marker(n, (index_type)-1) {}

        void begin_row(index_type row_id) {
            active_.clear();
            current_row_ = row_id;
        }
        inline void accumulate(index_type col, T v) {
            if (marker[col] != current_row_) {
                marker[col] = current_row_;
                val[col] = T{};
                active_.push_back(col);
            }
            val[col] += v;
        }
        std::vector<size_t>& used() { return active_; }
        index_type col_of(size_t slot) const { return (index_type)slot; }
        T val_of(size_t slot) const { return val[slot]; }
    };

    struct HashRowAccumulator {
        static constexpr index_type EMPTY = (index_type)-1;
        std::vector<index_type> slot_col;
        std::vector<T> slot_val;
        std::vector<size_t> used_;
        size_t mask_ = 0;

        static size_t next_pow2(size_t x) {
            size_t p = 1;
            while (p < x) p <<= 1;
            return p;
        }
        // Called once per thread with the max per-row bound in its chunk.
        // Distinct columns touched in a row are <= bound, and capacity is
        // 2*bound (rounded up to a power of two), so load factor stays
        // <= 50% and linear probing cannot degenerate into an infinite loop.
        void reserve_capacity(size_t bound) {
            size_t capacity = next_pow2(std::max<size_t>(4, bound * 2));
            slot_col.assign(capacity, EMPTY);
            slot_val.assign(capacity, T{});
            mask_ = capacity - 1;
        }
        void begin_row(index_type /*row_id*/) {
            for (size_t s : used_) slot_col[s] = EMPTY;  // O(k), not O(capacity)
            used_.clear();
        }
        static inline size_t mix(index_type x) {
            uint64_t h = x;
            h ^= h >> 16; h *= 0x85ebca6bULL;
            h ^= h >> 13; h *= 0xc2b2ae35ULL;
            h ^= h >> 16;
            return (size_t)h;
        }
        inline void accumulate(index_type col, T v) {
            size_t h = mix(col) & mask_;
            while (slot_col[h] != EMPTY && slot_col[h] != col) h = (h + 1) & mask_;
            if (slot_col[h] == EMPTY) {
                // The table only grows on this branch (existing-slot updates
                // never touch used_), so guarding the load-factor invariant here
                // costs O(k) per row instead of one branch per accumulate call.
                assert(used_.size() < (mask_ + 1) / 2);  // insert cannot saturate
                slot_col[h] = col;
                slot_val[h] = v;
                used_.push_back(h);
            } else {
                slot_val[h] += v;
            }
        }
        std::vector<size_t>& used() { return used_; }
        index_type col_of(size_t slot) const { return slot_col[slot]; }
        T val_of(size_t slot) const { return slot_val[slot]; }
    };

    // Shared body: fan out A-row i through B, prune to `limit`, emit Triplets.
    // Identical for both accumulator kinds via the used()/col_of()/val_of() interface.
    //
    // IMPORTANT: `used` is a reference to the accumulator's own bookkeeping
    // list. For HashRowAccumulator, begin_row() clears exactly the slots
    // recorded in that list on the NEXT row, so it must still list every
    // slot actually written this row when we're done here -- truncating it
    // to `limit` would silently leak the pruned-away slots (never marked
    // EMPTY again, never re-tracked), which fills the table with stale
    // entries after a few rows and makes accumulate()'s probe loop spin
    // forever once it's full. nth_element only reorders in place, so we
    // prune by adjusting how many *leading* entries we emit, not by
    // resizing the list itself.
    template <typename Acc>
    static void multiply_prune_row(size_type i, const CSRMatrix& A, const CSRMatrix& B,
                                    size_t limit, Acc& acc, BlockAllocator<Triplet>& allocator
#ifdef PHASE_PROFILE
                                    , phprof::RowTimer* rt = nullptr
#endif
                                    ) {
#ifdef PHASE_PROFILE
        uint64_t t0_row = rt ? phprof::tick() : 0;
#endif
        for (size_type idx_a = A.row_ptr_[i]; idx_a < A.row_ptr_[i + 1]; ++idx_a) {
            size_type k = A.col_idx_[idx_a];
            T val_a = A.values_[idx_a];
            for (size_type idx_b = B.row_ptr_[k]; idx_b < B.row_ptr_[k + 1]; ++idx_b) {
                acc.accumulate(B.col_idx_[idx_b], val_a * B.values_[idx_b]);
            }
        }
#ifdef PHASE_PROFILE
        if (rt) { rt->f1 += phprof::tick() - t0_row; t0_row = phprof::tick(); ++rt->rows; }
#endif

        auto& used = acc.used();
        size_t emit_count = used.size();
        if (emit_count > limit) {
            std::nth_element(used.begin(), used.begin() + limit, used.end(),
                [&](size_t a, size_t b) {
                    return std::abs(acc.val_of(a)) > std::abs(acc.val_of(b));
                });
            emit_count = limit;  // do NOT used.resize(...) -- see note above
        }

                // FASE 1: sort columns within this row so the fragment-CSR builder
        // receives (row, col) ordered streams. nth_element reorders by |v|,
        // not by col; with emit_count <= limit (small) this sort is cheap.
        // For DenseRowAccumulator, col_of(slot) = slot (already col-sorted),
        // so we skip the sort to avoid needless overhead.
        if constexpr (!std::is_same_v<Acc, DenseRowAccumulator>) {
            std::sort(used.begin(), used.begin() + emit_count,
                [&](size_t a, size_t b) {
                    return acc.col_of(a) < acc.col_of(b);
                });
        }
        for (size_t idx = 0; idx < emit_count; ++idx) {
            size_t slot = used[idx];
            T v = acc.val_of(slot);
            if (std::abs(v) > 1e-9) {
                Triplet* t = allocator.allocate();
                t->row = i; t->col = acc.col_of(slot); t->value = v; t->is_set = false;
#ifdef PHASE_PROFILE
                if (rt) ++rt->emitted;
#endif
            }
        }
#ifdef PHASE_PROFILE
        if (rt) rt->f2 += phprof::tick() - t0_row;
#endif
    }

public:
    // =========================================================================
    // Matrix Operations (Using optimized build)
    // =========================================================================

    static CSRMatrix multiply_and_prune(const CSRMatrix& A, const CSRMatrix& B, size_t limit) {
        if (A.cols_ != B.rows_) throw std::invalid_argument("Dimension mismatch");
#ifdef PHASE_PROFILE
        // A0: call-level ledger. MP_CALL wraps everything except the
        // allocator teardown, which gets its own phase so the residue of the
        // pipe ledger is a named number instead of an unexplained gap.
        uint64_t t0_mp; phprof::RUsageSnap r0_mp;
        phprof::serial_begin(phprof::P_MP_CALL, t0_mp, r0_mp);
        phprof::g().last_mult_cols_b = B.cols_;
        phprof::mem_point("mult_entry");
#endif

        CSRMatrix C(A.rows_, B.cols_, 0);
        int num_threads = omp_get_max_threads();
        
        // CRITICAL: Separate allocation to prevent false sharing
#ifdef PHASE_PROFILE
        uint64_t t0_as; phprof::RUsageSnap r0_as;
        phprof::serial_begin(phprof::P_ALLOC_SETUP, t0_as, r0_as);
#endif
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_ALLOC_SETUP, t0_as, r0_as);
        phprof::mem_point("after_alloc_setup");
#endif

        // Below this B.cols_/bound ratio the plain dense array is faster in
        // practice (hash probing overhead isn't paid back yet). Calibration
        // (single core, ra=rb=20, bound=400; Source/bench/bench_accumulator.cpp):
        //   ratio    25 (N=10k):  dense 0.95x   -> guard must stay dense here
        //   ratio   250 (N=100k): hash  1.16x
        //   ratio  2500 (N=1M):   hash  1.70x
        //   ratio  5000 (N=2M):   hash  2.19x
        //   ratio 125000 (N=50M): hash  3.64x
        // Hash already wins at ratio ~250 on the calibration host, so 200 is
        // a validated conservative default. Re-benchmark on the target
        // hardware / sparsity profile before trusting it blindly.
        constexpr size_t ADAPTIVE_RATIO = 200;

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;

            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

            // Real per-row fan-out bound for this thread's rows:
            // bound_i = sum_{k in row_i(A)} nnz_B(k)  ->  O(k^2), not O(n).
#ifdef PHASE_PROFILE
            // Phase-profile locals (stack): merged into the thread's
            // aligned slot once, at region exit.
            phprof::RowTimer rt_local;
            uint64_t reg_t0 = phprof::tick();
            uint64_t rows_local = 0, bound_sum_local = 0;
            uint64_t t_prep0 = reg_t0;   // A0: ACC_PREP = bound scan + acc ctor
#endif
            size_t max_bound = 0;
            for (size_t i = r_start; i < r_end; ++i) {
                size_t bound = 0;
                for (size_type idx_a = A.row_ptr_[i]; idx_a < A.row_ptr_[i+1]; ++idx_a) {
                    size_type k = A.col_idx_[idx_a];
                    bound += (B.row_ptr_[k+1] - B.row_ptr_[k]);
                }
                max_bound = std::max(max_bound, bound);
#ifdef PHASE_PROFILE
                bound_sum_local += bound;
#endif
            }

            bool use_hash = (size_t)B.cols_ > ADAPTIVE_RATIO * std::max<size_t>(max_bound, 1);

            if (use_hash) {
                HashRowAccumulator acc;
                acc.reserve_capacity(max_bound);
#ifdef PHASE_PROFILE
                phprof::accum_ticks(tid, phprof::P_ACC_PREP,
                                    phprof::tick() - t_prep0);
                phprof::counters()[tid].hash_threads = 1;
                phprof::mem_point_t0("after_acc_prep_hash");
#endif
                for (size_t i = r_start; i < r_end; ++i) {
                    acc.begin_row((index_type)i);
#ifdef PHASE_PROFILE
                    bool timed = (PHASE_PROFILE_ROW_SAMPLE == 1) ||
                                 (((i - r_start) % (size_t)PHASE_PROFILE_ROW_SAMPLE) == 0);
                    multiply_prune_row(i, A, B, limit, acc, allocator,
                                       timed ? &rt_local : nullptr);
                    ++rows_local;
#else
                    multiply_prune_row(i, A, B, limit, acc, allocator);
#endif
                }
            } else {
                DenseRowAccumulator acc(B.cols_);
#ifdef PHASE_PROFILE
                phprof::accum_ticks(tid, phprof::P_ACC_PREP,
                                    phprof::tick() - t_prep0);
                phprof::counters()[tid].dense_threads = 1;
                phprof::mem_point_t0("after_acc_prep_dense");
#endif
                for (size_t i = r_start; i < r_end; ++i) {
                    acc.begin_row((index_type)i);
#ifdef PHASE_PROFILE
                    bool timed = (PHASE_PROFILE_ROW_SAMPLE == 1) ||
                                 (((i - r_start) % (size_t)PHASE_PROFILE_ROW_SAMPLE) == 0);
                    multiply_prune_row(i, A, B, limit, acc, allocator,
                                       timed ? &rt_local : nullptr);
                    ++rows_local;
#else
                    multiply_prune_row(i, A, B, limit, acc, allocator);
#endif
                }
            }
#ifdef PHASE_PROFILE
            {
                uint64_t reg_t1 = phprof::tick();
                phprof::ThreadCtr& c_pp = phprof::counters()[tid];
                c_pp.ticks[phprof::P_MULT_TOTAL] += reg_t1 - reg_t0;
                c_pp.start[phprof::P_MULT_TOTAL] = reg_t0;
                c_pp.end[phprof::P_MULT_TOTAL]   = reg_t1;
                ++c_pp.calls[phprof::P_MULT_TOTAL];
                c_pp.ticks[phprof::P_F1_ACCUM] += rt_local.f1;
                c_pp.ticks[phprof::P_F2_PRUNE] += rt_local.f2;
                c_pp.calls[phprof::P_F1_ACCUM] += rt_local.rows;
                c_pp.calls[phprof::P_F2_PRUNE] += rt_local.rows;
                c_pp.rows_total += rows_local;
                c_pp.rows_f1    += rt_local.rows;
                c_pp.emitted    += rt_local.emitted;
                c_pp.bound_sum  += bound_sum_local;
                c_pp.max_bound   = std::max<uint64_t>(c_pp.max_bound,
                                                      (uint64_t)max_bound);
            }
            phprof::mem_point_t0("after_emit");
#endif
        }
        
        build_from_ordered_fragments(C, allocators, ranges);
#ifdef PHASE_PROFILE
        phprof::mem_point("mult_exit");
        phprof::serial_end(phprof::P_MP_CALL, t0_mp, r0_mp);
        {
            // Named teardown of the per-thread 2 MB allocator blocks, so the
            // pipe/mult ledger has no unattributed free() at scope exit.
            uint64_t t0_at; phprof::RUsageSnap r0_at;
            phprof::serial_begin(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
            allocators.clear();
            allocators.shrink_to_fit();
            phprof::serial_end(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
            phprof::mem_point("after_alloc_teardown");
        }
#endif
        return C;
    }

    template <typename BinaryOp>
    static CSRMatrix merge(const CSRMatrix& A, const CSRMatrix& B, BinaryOp op) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
#ifdef PHASE_PROFILE
        uint64_t t0_mc; phprof::RUsageSnap r0_mc;
        phprof::serial_begin(phprof::P_MERGE_CALL, t0_mc, r0_mc);
        phprof::mem_point("merge_entry");
#endif
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
#ifdef PHASE_PROFILE
        uint64_t t0_as; phprof::RUsageSnap r0_as;
        phprof::serial_begin(phprof::P_ALLOC_SETUP, t0_as, r0_as);
#endif
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_ALLOC_SETUP, t0_as, r0_as);
        phprof::mem_point("merge_after_alloc_setup");
#endif

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;
            
            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_merge(phprof::P_MERGE_LOOP);
#endif
            for (size_t i = r_start; i < r_end; ++i) {
                size_t idx_a = A.row_ptr_[i], end_a = A.row_ptr_[i+1];
                size_t idx_b = B.row_ptr_[i], end_b = B.row_ptr_[i+1];

                while (idx_a < end_a && idx_b < end_b) {
                    size_t ca = A.col_idx_[idx_a];
                    size_t cb = B.col_idx_[idx_b];
                    if (ca < cb) {
                        T res = op(A.values_[idx_a], T{});
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = ca; t->value = res;
                        }
                        idx_a++;
                    } else if (cb < ca) {
                        T res = op(T{}, B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = cb; t->value = res;
                        }
                        idx_b++;
                    } else {
                        T res = op(A.values_[idx_a], B.values_[idx_b]);
                        if (res != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = ca; t->value = res;
                        }
                        idx_a++; idx_b++;
                    }
                }
                
                while (idx_a < end_a) {
                    T res = op(A.values_[idx_a], T{});
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = A.col_idx_[idx_a]; t->value = res;
                    }
                    idx_a++;
                }
                
                while (idx_b < end_b) {
                    T res = op(T{}, B.values_[idx_b]);
                    if (res != T{}) {
                        Triplet* t = allocator.allocate();
                        t->row = i; t->col = B.col_idx_[idx_b]; t->value = res;
                    }
                    idx_b++;
                }
            }
        }
        
        build_from_ordered_fragments(C, allocators, ranges);
#ifdef PHASE_PROFILE
        phprof::mem_point("merge_exit");
        phprof::serial_end(phprof::P_MERGE_CALL, t0_mc, r0_mc);
        {
            uint64_t t0_at; phprof::RUsageSnap r0_at;
            phprof::serial_begin(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
            allocators.clear();
            allocators.shrink_to_fit();
            phprof::serial_end(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
        }
#endif
        return C;
    }

    static CSRMatrix combine(const CSRMatrix& A, const CSRMatrix& B) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
#ifdef PHASE_PROFILE
        uint64_t t0_cb; phprof::RUsageSnap r0_cb;
        phprof::serial_begin(phprof::P_COMBINE_CALL, t0_cb, r0_cb);
        phprof::mem_point("combine_entry");
#endif
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
#ifdef PHASE_PROFILE
        uint64_t t0_as; phprof::RUsageSnap r0_as;
        phprof::serial_begin(phprof::P_ALLOC_SETUP, t0_as, r0_as);
#endif
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_ALLOC_SETUP, t0_as, r0_as);
        phprof::mem_point("combine_after_alloc_setup");
#endif

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& allocator = *allocators[tid].ptr;
            
            size_t chunk = (A.rows_ + num_threads - 1) / num_threads;
            size_t r_start = std::min((size_t)A.rows_, (size_t)tid * chunk);
            size_t r_end   = std::min((size_t)A.rows_, (size_t)(tid + 1) * chunk);
            ranges[tid] = {r_start, r_end};

#ifdef PHASE_PROFILE
            phprof::PhaseMark pm_comb(phprof::P_COMBINE_LOOP);
#endif
            for (size_t i = r_start; i < r_end; ++i) {
                size_t nnz_a = A.row_ptr_[i+1] - A.row_ptr_[i];
                size_t nnz_b = B.row_ptr_[i+1] - B.row_ptr_[i];
                if (nnz_a == 0 || nnz_b == 0) continue;

                if (nnz_a < nnz_b) {
                    for (size_type idx = A.row_ptr_[i]; idx < A.row_ptr_[i+1]; ++idx) {
                        size_type col = A.col_idx_[idx];
                        T val_b = B.get(i, col);
                        if (val_b != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = col; t->value = A.values_[idx] * val_b;
                        }
                    }
                } else {
                    for (size_type idx = B.row_ptr_[i]; idx < B.row_ptr_[i+1]; ++idx) {
                        size_type col = B.col_idx_[idx];
                        T val_a = A.get(i, col);
                        if (val_a != T{}) {
                            Triplet* t = allocator.allocate();
                            t->row = i; t->col = col; t->value = val_a * B.values_[idx];
                        }
                    }
                }
            }
        }
        
        build_from_ordered_fragments(C, allocators, ranges);
#ifdef PHASE_PROFILE
        phprof::mem_point("combine_exit");
        phprof::serial_end(phprof::P_COMBINE_CALL, t0_cb, r0_cb);
        {
            uint64_t t0_at; phprof::RUsageSnap r0_at;
            phprof::serial_begin(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
            allocators.clear();
            allocators.shrink_to_fit();
            phprof::serial_end(phprof::P_ALLOC_TEARDOWN, t0_at, r0_at);
        }
#endif
        return C;
    }

    // =========================================================================
    // Utility Functions
    // =========================================================================
    
    size_t nnz() const { return values_.size(); }
    size_type rows() const { return rows_; }
    size_type cols() const { return cols_; }
    size_type row_nnz(size_type row) const { 
        return row_ptr_[row+1] - row_ptr_[row]; 
    }
    
    T get(size_type row, size_type col) const {
        if (row >= rows_) return T{};
        auto it = std::lower_bound(col_idx_.begin() + row_ptr_[row], 
                                   col_idx_.begin() + row_ptr_[row+1], col);
        if (it != col_idx_.begin() + row_ptr_[row+1] && *it == col) {
            return values_[std::distance(col_idx_.begin(), it)];
        }
        return T{};
    }

    template <typename Func>
    void for_each_in_row(size_type row, Func&& func) const {
        for (size_type i = row_ptr_[row]; i < row_ptr_[row+1]; ++i) {
            func(col_idx_[i], values_[i]);
        }
    }

    CSRMatrix transpose() const {
#ifdef PHASE_PROFILE
        uint64_t t0_tr; phprof::RUsageSnap r0_tr;
        phprof::serial_begin(phprof::P_TRANSPOSE, t0_tr, r0_tr);
        phprof::mem_point("transpose_entry");
#endif
        CSRMatrix result(cols_, rows_, values_.size());
        std::vector<size_type> counts(cols_, 0);
        for (auto c : col_idx_) counts[c]++;
        
        result.row_ptr_[0] = 0;
        for (size_type i = 0; i < cols_; ++i) {
            result.row_ptr_[i+1] = result.row_ptr_[i] + counts[i];
        }
        
        result.values_.resize(values_.size());
        result.col_idx_.resize(col_idx_.size());
        
        std::vector<size_type> current(cols_);
        for (size_type i = 0; i < cols_; ++i) current[i] = result.row_ptr_[i];
        
        for (size_type r = 0; r < rows_; ++r) {
            for (size_type j = row_ptr_[r]; j < row_ptr_[r+1]; ++j) {
                size_type c = col_idx_[j];
                size_type dest = current[c]++;
                result.values_[dest] = values_[j];
                result.col_idx_[dest] = r;
            }
        }
        
#ifdef PHASE_PROFILE
        phprof::mem_point("transpose_exit");
        phprof::serial_end(phprof::P_TRANSPOSE, t0_tr, r0_tr);
#endif
        result.finalized_ = true;
        return result;
    }
    
    void reset() {
        finalized_ = false;
        values_.clear();
        col_idx_.clear();
        std::fill(row_ptr_.begin(), row_ptr_.end(), 0);
    }

private:
    size_type rows_, cols_;
    bool finalized_;
    std::vector<T> values_;
    std::vector<index_type> col_idx_, row_ptr_;
    std::vector<Triplet> temp_triplets_;
};

#endif // SPARSE_MATRIX_HPP