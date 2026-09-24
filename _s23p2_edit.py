#!/usr/bin/env python3
# TEMP script -- S2.3 step 2 mechanical edits (remove the legacy + fragment-CSR
# builders, their knobs, hooks and TBB/<execution> dependency of the builder).
# Delete this file before committing.
import re
import sys

R = "/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR"


def read(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(p, s):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def rep(s, path, old, new, n=1, crlf=True):
    o = old.replace("\n", "\r\n") if crlf else old
    nn = new.replace("\n", "\r\n") if crlf else new
    k = s.count(o)
    if k != n:
        sys.exit("ABORT %s: %d hits (want %d) for %r" % (path, k, n, old[:60]))
    print("  ok %s: %.56s... x%d" % (path.split("/")[-1], old.strip(), n))
    return s.replace(o, nn)


def rsub(s, path, pattern, new, n=1):
    k = len(re.findall(pattern, s))
    if k != n:
        sys.exit("ABORT %s: regex %r matched %d (want %d)" % (path, pattern, k, n))
    print("  ok %s: regex %.40s... x%d" % (path.split("/")[-1], pattern, n))
    return re.sub(pattern, new, s, count=n)


def eq(t):
    return lambda x: x == t


def pre(t):
    return lambda x: x.startswith(t)


def isblank(x):
    return x.strip() == ""


def cut(lines, path, a, b, first_ok, last_ok):
    f = lines[a - 1].rstrip("\r\n")
    l = lines[b - 1].rstrip("\r\n")
    if not first_ok(f) or not last_ok(l):
        sys.exit("ABORT %s: cut %d-%d boundaries %r / %r" % (path, a, b, f, l))
    print("  cut %s lines %d-%d (%d lines)" % (path.split("/")[-1], a, b, b - a + 1))
    return lines[:a - 1] + lines[b:]


# ===========================================================================
# 1. SparseMatrix.hpp -- the engine itself
# ===========================================================================
p = R + "/Source/src/Core/hpp/SparseMatrix.hpp"
s = read(p)
lines = s.splitlines(keepends=True)
if len(lines) != 1944:
    sys.exit("ABORT: SparseMatrix.hpp has %d lines (want 1944)" % len(lines))

# bottom-up block deletions (line numbers are the pre-edit ones)
lines = cut(lines, p, 1170, 1327, eq("private:"), eq("    }"))       # buildCSRParallel
lines = cut(lines, p, 794, 927, pre("    // ===="), isblank)         # legacy builder
lines = cut(lines, p, 521, 689, pre("    // ===="), isblank)         # fragment builder
lines = cut(lines, p, 503, 516, eq("#ifdef PHASE_PROFILE"), isblank)  # ABHook
s = "".join(lines)

# #include <execution> (builder-side par_unseq sort only user)
if s.count("#include <execution>") != 1:
    sys.exit("ABORT: unexpected '#include <execution>' count")
s = rsub(s, p, r"#include <execution>[ \t]*\r\n", "")

s = rep(s, p, """//   * Build sub-phases: B0 temp_triplets_ resize (serial), B1 copy,
//     B2 sort, B3 histogram+reduction+prefix, B4 values_/col_idx_ resize
//     (serial), B5 fill. Serial phases record wall ticks + getrusage
//     deltas (user/sys time, page faults); parallel build regions record
//     an aggregated rusage delta around the region. The multiply/merge/
//     combine row loops record ticks + per-thread end stamps only.
""", """//   * Build sub-phases of the single builder v2 (S2.3 step 2): B3_HIST = V1
//     per-thread histogram, B3_PREFIX = V2 prefix + cursors, B5_FILL = V3
//     scatter, V4_ROWSORT = per-row sort + coalesce, V5_COMPACT = serial
//     compaction. Serial phases record wall ticks + getrusage deltas
//     (user/sys time, page faults); parallel build regions record an
//     aggregated rusage delta around the region. The multiply/merge/combine
//     row loops record ticks + per-thread end stamps only.
""")

# enum: retag the retired ids, keep their slot (ledger column stability)
for old, new in [
    ("    P_B1_COPY,         // build: parallel triplet copy",
     "    P_B1_COPY,         // RETIRED (S2.3 p2): legacy builder triplet copy"),
    ("    P_B3_HIST,         // build: histogram (parallel region only)",
     "    P_B3_HIST,         // v2 V1: per-thread row histogram (parallel)"),
    ("    P_B5_FILL,         // build: parallel fill of values_/col_idx_",
     "    P_B5_FILL,         // v2 V3: parallel scatter into values_/col_idx_"),
    ("    P_B0_TEMP_RESIZE,  // build: serial resize of temp_triplets_",
     "    P_B0_TEMP_RESIZE,  // RETIRED (S2.3 p2): legacy temp_triplets_ resize"),
    ("    P_B2_SORT,         // build: global sort (wall; TBB threads inside)",
     "    P_B2_SORT,         // RETIRED (S2.3 p2): legacy global sort (TBB)"),
    ("    P_B4_VALS_RESIZE,  // build: serial resize of values_/col_idx_",
     "    P_B4_VALS_RESIZE,  // RETIRED (S2.3 p2): legacy output resize"),
    ("    P_B3_SETUP,        // build: thread_row_counts + row_counts alloc (serial)",
     "    P_B3_SETUP,        // RETIRED (S2.3 p2): legacy histogram setup"),
    ("    P_B3_RED,          // build: parallel reduction of thread histograms",
     "    P_B3_RED,          // RETIRED (S2.3 p2): legacy histogram reduction"),
    ("    P_B3_PREFIX,       // build: serial prefix sum into row_ptr_",
     "    P_B3_PREFIX,       // v2 V2: prefix sum + per-thread cursors"),
    ("    P_BUILD_FINISH,    // build: release of the shared sorted triplet buffer",
     "    P_BUILD_FINISH,    // RETIRED (S2.3 p2): legacy temp release"),
]:
    s = rep(s, p, old + "\n", new + "\n")

s = rep(s, p, """};
constexpr int MAX_THREADS = 128;
""", """};

// RETIRED ids (B0/B1/B2/B3_SETUP/B3_RED/B4/BUILD_FINISH) keep their slot and
// their names[] entry on purpose: the PHASEPROFILE ledger stays column-stable
// against the pre-S2.3p2 captures, where they simply report zero now.
constexpr int MAX_THREADS = 128;
""")
write(p, s)
print("SparseMatrix.hpp: stage 1 written")
s = read(p)
s = rep(s, p, """    // Builder v2 (S2.1) -- one order-agnostic builder, no global temp buffer.
""", """    // Builder v2 -- the SINGLE builder (S2.3 step 2 consolidation).
    //
    // It replaces both alternatives removed in that step: the legacy
    // global-sort builder (temp_triplets_ + std::sort(par_unseq), which S2.1
    // had kept as the A/B reference) and the fragment-CSR builder
    // (build_from_ordered_fragments), whose emission-order invariant the
    // callers could not guarantee -- multiply_and_prune's LHRR symmetric case
    // emits rows outside the emitting thread's range. Every producer in the
    // engine (multiply_and_prune, merge, combine, the methods) routes here.
""")
s = rep(s, p, """    //   * Memory: NO copy of the emitted stream. The legacy builder allocated
    //     16 B per emitted record (921.6 MB on the ALOI position build); v2
    //     needs T*rows*4 B of cursors plus the CSR output itself.
""", """    //   * Memory: NO copy of the emitted stream. v2 needs NT*rows*size_type B
    //     of cursors plus the CSR output itself (the retired legacy builder
    //     first materialized the whole stream: 16 B per record -- 921.6 MB on
    //     the ALOI position build).
""")
s = rep(s, p, """    //   * Determinism: bit-identical for a fixed thread count. Across thread
    //     counts the summation order of a cell with >= 3 duplicate records is
    //     unspecified -- the legacy path was equally unspecified (std::sort is
    //     unstable and par_unseq is not deterministic).
""", """    //   * Determinism: bit-identical for a fixed thread count. Across thread
    //     counts only the summation order of a cell with >= 3 duplicate
    //     records is unspecified (last bits of the sum).
""")
s = rep(s, p, """        // it is the same summation order the legacy path had before its sort.
""", """        // it is deterministic for a fixed thread count.
""")
s = rep(s, p, """        // FASE 1 (corrected): sort columns within this row so the fragment-CSR
        // builder receives (row, col) ordered streams. nth_element reorders by
        // |v|, not by col -- and even the un-pruned order is the accumulator's
        // FIRST-TOUCH order, which is not ascending whenever row i of A has
        // more than one k and the k-th rows of B overlap out of column order.
        // Both branches must sort: get() is lower_bound and merge() is a
        // two-pointer, so an unsorted row silently corrupts every consumer.
""", """        // Sort columns within this row: nth_element reorders by |v|, not by
        // col, and even the un-pruned order is the accumulator's FIRST-TOUCH
        // order, which is not ascending whenever row i of A has more than one
        // k and the k-th rows of B overlap out of column order. v2 tolerates
        // any order, but the emitted order has to stay deterministic (the
        // per-row coalesce sums duplicate records in emission order) and the
        // consumers binary-search, so both branches sort.
""")
s = rep(s, p, """        // Order invariant of the emitted stream (debug builds only): the
        // fragment-CSR builder and every consumer depend on it.
""", """        // Emitted-order invariant (debug builds only): every consumer
        // binary-searches the row, so this must hold.
""")
# the three production call sites (multiply_and_prune, merge, combine) -> v2
s = rep(s, p, "build_from_ordered_fragments(C, allocators, ranges);",
        "build_from_partial_results(C, allocators, ranges);", n=3)
# the member that only the legacy builder used
s = rep(s, p, """    std::vector<index_type> col_idx_, row_ptr_;
    std::vector<Triplet> temp_triplets_;
""", """    std::vector<index_type> col_idx_, row_ptr_;
""")
write(p, s)
print("SparseMatrix.hpp: stage 2 written")

# ===========================================================================
# 3. bench_phase.cpp -- driver
# ===========================================================================
p = R + "/Source/bench/bench_phase.cpp"
s = read(p)
s = rep(s, p, """// Threads: OMP_NUM_THREADS / PHASE_THREADS for OpenMP; the same count is also
// applied to TBB (tbb::global_control) because std::execution::par_unseq in
// the build sort ignores OMP_NUM_THREADS. The effective affinity mask and the
// TBB max_concurrency are printed on the TSET line.
""", """// Threads: OMP_NUM_THREADS / PHASE_THREADS. S2.3 step 2 removed the
// std::execution::par_unseq build sort, so OpenMP is the only thread source
// and no TBB arena is created or configured. The effective affinity mask is
// printed on the TSET line.
""")
s = rep(s, p, """//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp \\
//       -o /tmp/bench_phase -ltbb
""", """//   g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/bench/bench_phase.cpp \\
//       -o /tmp/bench_phase
""")
s = rep(s, p, """#include <vector>

#include <tbb/global_control.h>
#include <tbb/task_arena.h>

#ifdef PHASE_PROFILE
#include "phase_ab.hpp"   // S2.3: fragment-CSR vs v2, same emitted base
#endif

""", """#include <vector>

""")
s = rep(s, p, "// A0 helpers: wall clock, per-statement ledger, affinity/TBB reporting.\n",
        "// A0 helpers: wall clock, per-statement ledger, affinity reporting.\n")
s = rep(s, p, """static void print_tset(int omp_threads) {
    std::printf("TSET,omp_threads=%d,tbb_max=%d,affinity_omp=%s,"
                "affinity_at_entry=%s,req_mask=%s,"
                "cpus=%ld,max_concurrency=%zu,pid=%d\\n",
                omp_threads, omp_get_max_threads(), affinity_mask().c_str(),
                g_affinity_at_entry.c_str(), env_or("PHASE_MASK", "unset").c_str(),
                sysconf(_SC_NPROCESSORS_ONLN),
                (size_t)tbb::this_task_arena::max_concurrency(), (int)getpid());
}
""", """static void print_tset(int omp_threads) {
    // tbb_max is gone (S2.3 p2: the builder no longer sorts through
    // std::execution/par_unseq). The field is kept as an explicit "none" so an
    // older parser fails loudly on the value instead of reading a stale
    // number; max_concurrency now reports the OpenMP pool.
    std::printf("TSET,omp_threads=%d,tbb_max=none,affinity_omp=%s,"
                "affinity_at_entry=%s,req_mask=%s,"
                "cpus=%ld,max_concurrency=%zu,pid=%d\\n",
                omp_threads, omp_get_max_threads(), affinity_mask().c_str(),
                g_affinity_at_entry.c_str(), env_or("PHASE_MASK", "unset").c_str(),
                sysconf(_SC_NPROCESSORS_ONLN),
                (size_t)omp_get_max_threads(), (int)getpid());
}
""")
s = rep(s, p, """// S2.1 A/B knobs and the canonical dump.
//   PHASE_BUILDER=legacy|v2   builder used by every driver-side build
//   PHASE_ROWSORT=idx|zip     v2 per-row sort strategy
""", """// Driver knobs and the canonical dump.
//   PHASE_BUILDER, PHASE_FRAGAB   GONE (S2.3 step 2): the legacy and
//                             fragment-CSR builders were removed, so every
//                             driver-side build is v2. Setting either prints a
//                             warning and is otherwise ignored -- a stale
//                             harness must not silently get the wrong arm.
//   PHASE_ROWSORT=idx|zip     v2 per-row sort strategy
""")
s = rep(s, p, "static int g_use_legacy = 0;\n", "")
write(p, s)
print("bench_phase.cpp: stage 1 written")

# ===========================================================================
# 4. bench_phase.cpp -- driver (stage 2)
# ===========================================================================
p = R + "/Source/bench/bench_phase.cpp"
s = read(p)
s = rep(s, p, """// S2.3 (PHASE_FRAGAB): under the knob, the natural v2 build is timed and handed
// to phaseab together with the LIVE base -- it runs the per-matrix
// fragment-vs-v2 interleave (contract probe / charged normalization /
// 3+3 samples / structure+value check). The one-shot synthetic "ab" tag and
// CPRR's "W" are out of scope: the six real matrices are MM, H, S, Srev, Stot,
// result (LHRR rep order, seq 1..6).
static void build_into(M& m, std::vector<M::PaddedAllocator>& allocators,
                       std::vector<std::pair<idx_t, idx_t>>& ranges,
                       const char* tag) {
    if (g_use_legacy) {
        M::build_from_partial_results_legacy(m, allocators, ranges);
    } else {
#ifdef PHASE_PROFILE
        double ab_t = now_ms();
        M::build_from_partial_results(m, allocators, ranges);
        if (phaseab::on && std::string(tag) != "W" &&
            std::string(tag) != "ab")
            phaseab::after_partial(m, allocators, ranges, tag,
                                   now_ms() - ab_t);
#else
        M::build_from_partial_results(m, allocators, ranges);
#endif
    }
    dump_matrix(m, tag);
}
""", """// S2.3 step 2: the driver-side builder is the single v2 path
// (build_from_partial_results). PHASE_BUILDER/PHASE_FRAGAB and the
// phase_ab.hpp interleave were removed with the legacy and fragment-CSR
// builders; the canonical dump below is unchanged.
static void build_into(M& m, std::vector<M::PaddedAllocator>& allocators,
                       std::vector<std::pair<idx_t, idx_t>>& ranges,
                       const char* tag) {
    M::build_from_partial_results(m, allocators, ranges);
    dump_matrix(m, tag);
}
""")
s = rep(s, p, """        phprof::reset();
        phaseab::new_rep();   // S2.3: seq 1..6 restarts each rep
""", """        phprof::reset();
""")
s = rep(s, p, """    // S2.1 A/B knobs (see the dump_matrix comment block).
    g_use_legacy = (env_or("PHASE_BUILDER", "v2") == std::string("legacy")) ? 1 : 0;
""", """    // S2.3 step 2: PHASE_BUILDER/PHASE_FRAGAB were removed together with the
    // builders they selected (see the dump_matrix comment block). A stale
    // harness that still exports them gets a loud warning instead of the
    // wrong arm.
    if (const char* b = std::getenv("PHASE_BUILDER"))
        std::fprintf(stderr, "warning: PHASE_BUILDER=%s ignored (S2.3 p2: "
                     "builder is v2 only)\\n", b);
    if (const char* b = std::getenv("PHASE_FRAGAB"))
        std::fprintf(stderr, "warning: PHASE_FRAGAB=%s ignored (S2.3 p2: the "
                     "fragment-CSR builder was removed)\\n", b);
""")
s = rep(s, p, """    int g_ab = 0;  // PHASE_FRAGAB=1: S2.3 fragment-CSR-vs-v2 A/B active
#ifdef PHASE_PROFILE
    if (env_or("PHASE_FRAGAB", "0") == std::string("1")) {
        g_ab = 1;
        phaseab::enable();
        M::ab_set_hook(&phaseab::after_frag<M>);
    }
#endif
    std::printf("ABKNOBS,builder=%s,rowsort=%s,dumpdir=%s,rkout=%s,validate=%d,fragab=%d\\n",
                g_use_legacy ? "legacy" : "v2",
                M::v2_row_sort_mode() ? "zip" : "idx",
                g_dump_dir.empty() ? "(none)" : g_dump_dir.c_str(),
                g_rk_out.empty() ? "(none)" : g_rk_out.c_str(), g_validate,
                g_ab);
""", """    std::printf("ABKNOBS,builder=v2,rowsort=%s,dumpdir=%s,rkout=%s,validate=%d\\n",
                M::v2_row_sort_mode() ? "zip" : "idx",
                g_dump_dir.empty() ? "(none)" : g_dump_dir.c_str(),
                g_rk_out.empty() ? "(none)" : g_rk_out.c_str(), g_validate);
""")
s = rep(s, p, """    // Thread control: OpenMP from PHASE_THREADS/OMP_NUM_THREADS, and the SAME
    // count on TBB, because the build sort (std::execution::par_unseq) runs on
    // the TBB arena and ignores OMP_NUM_THREADS.
""", """    // Thread control: OpenMP from PHASE_THREADS/OMP_NUM_THREADS. S2.3 step 2
    // removed the std::execution::par_unseq build sort, so this is the only
    // thread source (no TBB arena is created or configured).
""")
s = rep(s, p, """    omp_set_num_threads(omp_threads);
    std::unique_ptr<tbb::global_control> tbb_gc =
        std::make_unique<tbb::global_control>(
            tbb::global_control::max_allowed_parallelism, (size_t)omp_threads);

""", """    omp_set_num_threads(omp_threads);

""")
write(p, s)
print("bench_phase.cpp: stage 2 written")

# ===========================================================================
# 5. tests + suite runner
# ===========================================================================
p = R + "/Source/tests/test_csr_ops.cpp"
s = read(p)
lines = s.splitlines(keepends=True)
if len(lines) != 702:
    sys.exit("ABORT: test_csr_ops.cpp has %d lines (want 702)" % len(lines))
lines = cut(lines, p, 524, 589, eq("        {"), isblank)   # C20 vs the legacy builder
s = "".join(lines)
s = rep(s, p, """// Characterization suite for the CSR helpers that Fase 1 (fragment-CSR) will
// rewrite the producers of: build_from_partial_results, merge, combine and
// transpose (Source/src/Core/hpp/SparseMatrix.hpp).
""", """// Characterization suite for the CSR helpers of the engine: the single builder
// v2 (build_from_partial_results, S2.3 step 2) plus merge, combine and
// transpose (Source/src/Core/hpp/SparseMatrix.hpp).
""")
s = rep(s, p, "//       Source/tests/test_csr_ops.cpp -o /tmp/test_csr_ops -ltbb\n",
        "//       Source/tests/test_csr_ops.cpp -o /tmp/test_csr_ops\n")
s = rep(s, p, """             "not: C8 must stay in sync with the fragment-CSR builder.");
""", """             "not: C8 pins the merge contract every producer shares.");
""")
s = rep(s, p, """    // ascending. Regression guard for the fragment-CSR rewrite (Fase 1): the
""", """    // ascending. Regression guard: the
""")
s = rep(s, p, """        // C19-C22: builder v2 (S2.1). The contract under test: any emission
        // order, any thread (rows may leave the emitting thread's range),
        // is_set semantics, and a per-row sort that keeps (col, value) paired.
""", """        // C19, C21, C22: builder v2 -- the single builder since S2.3 step 2
        // (C20, which compared it against the retired legacy builder, is gone
        // with it). The contract under test: any emission order, any thread
        // (rows may leave the emitting thread's range), is_set semantics, and
        // a per-row sort that keeps (col, value) paired.
""")
s = rep(s, p, """            NOTE("C19: v2 carries the is_set flag through the scatter; the "
                 "fragment-CSR path ignores it (documented gap).");
""", """            NOTE("C19: v2 carries the is_set flag through the scatter and the "
                 "per-row coalesce honours it (replace vs sum, in emission "
                 "order).");
""")
s = rep(s, p, """            // C21: the LHRR exception case -- a thread emits records for rows
            // OUTSIDE its range (t2->row = neighbor in the symmetric build).
            // v2 must not depend on the range invariant at all; the
            // fragment-CSR path would reject this stream.
""", """            // C21: the LHRR case -- a thread emits records for rows OUTSIDE
            // its range (t2->row = neighbor in the symmetric build). It is why
            // the single builder must not depend on a range invariant, and why
            // the fragment-CSR path that required one was removed (S2.3 p2).
""")
write(p, s)
print("test_csr_ops.cpp written")

p = R + "/Source/tests/run_all.sh"
s = read(p)
s = rep(s, p, "# Requires: g++, libtbb  (Debian: apt install g++ libtbb-dev)\n",
        "# Requires: g++ with OpenMP (libgomp) -- nothing else. S2.3 step 2\n"
        "# removed the builder's std::execution/par_unseq sort and the TBB arena,\n"
        "# so these binaries link no TBB.\n", crlf=False)
s = rep(s, p, " -ltbb", "", n=3, crlf=False)
write(p, s)
print("run_all.sh written")

p = R + "/Source/tests/test_multiply_and_prune.cpp"
s = read(p)
s = rep(s, p, "//       Source/tests/test_multiply_and_prune.cpp -o /tmp/test_map -ltbb\n",
        "//       Source/tests/test_multiply_and_prune.cpp -o /tmp/test_map\n", crlf=False)
write(p, s)
print("test_multiply_and_prune.cpp written")

p = R + "/Source/bench/bench_e2e.cpp"
s = read(p)
s = rep(s, p, """// (accumulator + BlockAllocator + parallel sort + CSR build) against a
""", """// (accumulator + BlockAllocator + the v2 CSR build) against a
""", crlf=False)
s = rep(s, p, "//       -o /tmp/bench_e2e -ltbb\n", "//       -o /tmp/bench_e2e\n",
        crlf=False)
write(p, s)
print("bench_e2e.cpp written")

p = R + "/Source/bench/build_s23.sh"
s = read(p)
s = rep(s, p, "#!/bin/bash\n", """#!/bin/bash
# S2.3 passo 2: builder unico v2 -- sem <execution>/TBB, portanto sem -ltbb.
""")
s = rep(s, p, "    Source/bench/bench_phase.cpp -o /home/andre/udlf_bin/bp_s23 -ltbb\n",
        "    Source/bench/bench_phase.cpp -o /home/andre/udlf_bin/bp_s23\n")
write(p, s)
print("build_s23.sh written")

# ===========================================================================
# 6. step-1 harnesses: keep them in the tree, mark them historical
# ===========================================================================
p = R + "/Source/bench/phase_ab.hpp"
s = read(p)
s = rep(s, p, "// phase_ab.hpp -- S2.3 step 1: same-emitted-base A/B (fragment-CSR vs v2).\n",
        """// phase_ab.hpp -- S2.3 step 1: same-emitted-base A/B (fragment-CSR vs v2).
//
// HISTORICAL (S2.3 step 2): NOT included by any build anymore. The
// fragment-CSR builder it drives (CSRMatrix::build_from_ordered_fragments) and
// the bench hook (CSRMatrix::ab_set_hook) were removed in step 2, so this
// header only compiles against commit 1f56845 or earlier -- re-running the
// step-1 campaign means checking that revision out together with its driver.
""")
write(p, s)
print("phase_ab.hpp written")

p = R + "/Source/bench/s23_run.sh"
s = read(p)
s = rep(s, p, "#!/bin/bash\n", """#!/bin/bash
# HISTORICO (S2.3 passo 2): o builder fragment-CSR e o hook ab_set_hook foram
# removidos. O driver atual ignora PHASE_BUILDER/PHASE_FRAGAB (com aviso no
# stderr) e nao emite FRAG*; para reexecutar esta campanha use o driver do
# commit 1f56845 ou anterior (build_s23 pre-passo-2).
""")
write(p, s)
print("s23_run.sh written")

p = R + "/Source/bench/s23_agg.py"
s = read(p)
s = rep(s, p, '"""S2.3 step-1 aggregator: fragment-CSR vs v2 on the SAME emitted base.\n',
        '''"""S2.3 step-1 aggregator: fragment-CSR vs v2 on the SAME emitted base.

HISTORICO (S2.3 passo 2): esta campanha exige o driver ANTERIOR a remocao
(commit <= 1f56845). O builder fragment-CSR e o hook PHASE_FRAGAB nao existem
mais, e o driver atual nao emite ABKNOBS fragab nem linhas FRAG* -- o gate
abaixo (builder=v2 + fragab=1) nao teria como passar.
''')
write(p, s)
print("s23_agg.py written")

p = R + "/Source/bench/s22b_run.sh"
s = read(p)
s = rep(s, p, "#!/bin/bash\n", """#!/bin/bash
# HISTORICO (S2.3 passo 2): o braco legacy nao existe mais -- PHASE_BUILDER e
# ignorado pelo driver atual (aviso no stderr) e todo build e v2. Reexecutar
# este A/B exige o driver do commit 1f56845 ou anterior.
""")
write(p, s)
print("s22b_run.sh written")

p = R + "/Source/bench/s2_dump_matrix.sh"
s = read(p)
note = ("# S2.3 p2: PHASE_BUILDER is ignored by the post-removal driver (the\n"
        "# legacy arm is gone); this harness needs commit <= 1f56845.\n")
s = rsub(s, p, r"(?m)^[ \t]*PHASE_BUILDER=\$b", lambda m: note + m.group(0))
write(p, s)
print("s2_dump_matrix.sh written")

print("ALL EDITS APPLIED")
