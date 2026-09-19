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
enum : int {
    P_F1_ACCUM = 0,    // multiply: per-row accumulate (timed rows only)
    P_F2_PRUNE,        // multiply: per-row nth_element prune + emit
    P_MERGE_LOOP,      // merge(): per-thread row loop
    P_COMBINE_LOOP,    // combine(): per-thread row loop
    P_B1_COPY,         // build: parallel triplet copy
    P_B3_HIST,         // build: histogram + reduction + serial prefix sum
    P_B5_FILL,         // build: parallel fill of values_/col_idx_
    P_B0_TEMP_RESIZE,  // build: serial resize of temp_triplets_
    P_B2_SORT,         // build: global sort (wall; TBB threads inside)
    P_B4_VALS_RESIZE,  // build: serial resize of values_/col_idx_
    P_TRANSPOSE,       // transpose() whole (serial)
    P_MULT_TOTAL,      // multiply_and_prune: per-thread whole-region wall
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
};
inline Globals& g() { static Globals x; return x; }

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
        "final_kb=%ld,build_calls=%llu\n",
        G.hwm_before_build_max, G.hwm_after_build_max, G.hwm_final,
        (unsigned long long)G.build_calls);

    static const char* names[P_MAX] = {
        "F1_ACCUM", "F2_PRUNE", "MERGE_LOOP", "COMBINE_LOOP",
        "B1_COPY", "B3_HIST", "B5_FILL",
        "B0_TEMP_RESIZE", "B2_SORT", "B4_VALS_RESIZE", "TRANSPOSE",
        "MULT_TOTAL"
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

    static void build_from_partial_results(
        CSRMatrix& C, 
        std::vector<PaddedAllocator>& allocators,
        const std::vector<std::pair<size_type, size_type>>& row_ranges
    ) {
        if (C.finalized_) throw std::logic_error("Matrix finalized");
#ifdef PHASE_PROFILE
        {   long h = phprof::vmhwm_kb();
            if (h > phprof::g().hwm_before_build_max)
                phprof::g().hwm_before_build_max = h;
            ++phprof::g().build_calls;
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
        std::vector<Triplet>().swap(C.temp_triplets_);
    }

private:
    static void buildCSRParallel(CSRMatrix& C, size_t total_elements, 
                                 const std::vector<std::pair<size_type, size_type>>& row_ranges,
                                 int num_threads) {
        
        // A. NUMA-aware histogram (each thread on its own memory)
        std::vector<std::vector<size_type>> thread_row_counts;
        thread_row_counts.resize(num_threads);
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
#endif
        // B. Parallel reduction
#ifdef PHASE_PROFILE
        phprof::RUsageSnap r0_red = phprof::rsnap();
#endif
        std::vector<size_type> row_counts(C.rows_, 0);
        
        #pragma omp parallel for
        for (size_t r = 0; r < C.rows_; ++r) {
            size_type sum = 0;
            for (int t = 0; t < num_threads; ++t) {
                sum += thread_row_counts[t][r];
            }
            row_counts[r] = sum;
        }

#ifdef PHASE_PROFILE
        phprof::ru_end(phprof::P_B3_HIST, r0_red);
#endif
        // C. Prefix sum (Sequential but fast)
#ifdef PHASE_PROFILE
        uint64_t t0_pre; phprof::RUsageSnap r0_pre;
        phprof::serial_begin(phprof::P_B3_HIST, t0_pre, r0_pre);
#endif
        C.row_ptr_[0] = 0;
        for (size_t i = 0; i < C.rows_; ++i) {
            C.row_ptr_[i+1] = C.row_ptr_[i] + row_counts[i];
        }
        
#ifdef PHASE_PROFILE
        phprof::serial_end(phprof::P_B3_HIST, t0_pre, r0_pre);
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

        CSRMatrix C(A.rows_, B.cols_, 0);
        int num_threads = omp_get_max_threads();
        
        // CRITICAL: Separate allocation to prevent false sharing
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

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
            }
#endif
        }
        
        build_from_partial_results(C, allocators, ranges);
        return C;
    }

    template <typename BinaryOp>
    static CSRMatrix merge(const CSRMatrix& A, const CSRMatrix& B, BinaryOp op) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

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
        
        build_from_partial_results(C, allocators, ranges);
        return C;
    }

    static CSRMatrix combine(const CSRMatrix& A, const CSRMatrix& B) {
        if (A.rows_ != B.rows_) throw std::invalid_argument("Dim mismatch");
        CSRMatrix C(A.rows_, A.cols_, 0);

        int num_threads = omp_get_max_threads();
        std::vector<PaddedAllocator> allocators;
        allocators.reserve(num_threads);
        for (int i = 0; i < num_threads; ++i) {
            allocators.emplace_back();
        }
        std::vector<std::pair<size_type, size_type>> ranges(num_threads);

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
        
        build_from_partial_results(C, allocators, ranges);
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