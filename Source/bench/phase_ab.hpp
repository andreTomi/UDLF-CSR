// phase_ab.hpp -- S2.3 step 1: same-emitted-base A/B (fragment-CSR vs v2).
//
// Included ONLY by bench_phase.cpp under #ifdef PHASE_PROFILE (self-guarded
// here as well): the test suites and production binaries never see this file.
// Enabled at runtime with PHASE_FRAGAB=1; inert otherwise.
//
// Per natural build of the real LHRR pipeline (seq 1..6 = MM, H, S, Srev,
// Stot, result -- partial-results builds arrive from bench build_into via
// phaseab::after_partial, fragment builds from the CSRMatrix::ABHook function
// pointer registered as after_frag):
//   1. FRAGPROBE   -- counts every fragment-contract clause on the LIVE base
//        (C.1 in-band rows, C.2 (row,col) ascending, C.3 adjacent duplicates
//        coalesced, plus is_set). Counted, NOT asserted: survives -DNDEBUG
//        and yields table evidence instead of aborting the campaign.
//   2. FRAGPREP    -- if the contract is missing, a CHARGED normalization
//        (bucket by owning thread + stable sort (row,col) + SUM-coalesce)
//        rebuilds a contract-satisfying base; prep_ms reported apart. The v2
//        arm never needs prep: its contract accepts the base as emitted
//        (unpartitioned, unsorted, duplicate-carrying).
//        Aborts (FRAGREJECT, C19) when a duplicate group touches
//        is_set=true: after sorting, the replace/sum chain of v2 depends on
//        record order and the fragment writer cannot reproduce it.
//   3. FRAGAB      -- 3 interleaved samples per builder over the IDENTICAL
//        base (natural first, then frag/v2 alternating). Both builders only
//        READ the allocators (v2: for_each with const Triplet&; frag: same)
//        -- proven in S2.3 and a precondition of this harness.
//   4. FRAGABCHECK -- natural output vs the first counterpart build:
//        identical structure (row_nnz per row + column sequence) and values
//        within 1e-6 relative (S2 acceptance note for >=3-record sum cells).
//
// Coalescing rule replicated from V4 (SparseMatrix.hpp:1084-1094): equal
// (row,col) cells SUM their values (is_set cells replace instead -- which is
// exactly why duplicate groups touching is_set are rejected, C19).
//
// Contamination note: the extra measurement builds also record PHASEPROFILE
// rows, so ledger lines from PHASE_FRAGAB runs must not be aggregated
// (s22b-style); this campaign consumes FRAG* + REP/REPRU/DONE only.
#ifndef PHASE_AB_HPP
#define PHASE_AB_HPP

#ifdef PHASE_PROFILE

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace phaseab {

// Session state. `seq` counts the NATURAL builds of the current rep (scratch
// builds triggered by measurement are fenced by `in_`); `rep` mirrors the
// driver's 0-based rep counter -- the aggregator discards rep 0, the standing
// policy of every campaign since S1.
inline bool on  = false;
inline bool in_ = false;   // re-entry guard: scratch builds from measurement
inline int  seq = 0;
inline int  rep = -1;

inline void enable()  { on = true; }
inline void new_rep() { seq = 0; ++rep; }

inline double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ---- 1. contract probe ----------------------------------------------------
// Walks each allocator's triplets in emitted order against the owning band
// row_ranges[tid] = [first, last). Adjacent-duplicate counting (C.3) is exact
// on contract-conforming streams; on unordered bases it can only undercount --
// fine: the normalization does a full census while sorting.
template <class M>
struct Probe {
    uint64_t emitted = 0, band_viol = 0, order_viol = 0, dupes = 0,
             is_set_true = 0;
    bool needs_norm() const { return band_viol || order_viol || dupes; }
};

template <class M>
Probe<M> probe(const std::vector<typename M::PaddedAllocator>& alloc,
               const std::vector<std::pair<typename M::size_type,
                                           typename M::size_type>>& rng) {
    using Triplet    = typename M::Triplet;
    using size_type  = typename M::size_type;
    using index_type = typename M::index_type;
    Probe<M> p;
    for (size_t t = 0; t < alloc.size(); ++t) {
        const size_type lo = rng[t].first, hi = rng[t].second;
        size_type  pr = 0;
        index_type pc = 0;
        bool first = true;
        alloc[t].ptr->for_each([&](const Triplet& x) {
            ++p.emitted;
            if (x.is_set) ++p.is_set_true;
            if (x.row < lo || x.row >= hi) ++p.band_viol;
            if (!first) {
                if (x.row < pr || (x.row == pr && x.col < pc))
                    ++p.order_viol;
                else if (x.row == pr && x.col == pc)
                    ++p.dupes;
            }
            pr = x.row; pc = x.col; first = false;
        });
    }
    return p;
}

// ---- 4. structure + value check (public read API only) --------------------
template <class M>
void check(const M& a, const M& b, int r, int s, const char* tag,
           const char* kind) {
    using size_type  = typename M::size_type;
    using index_type = typename M::index_type;
    using value_type = typename M::value_type;
    bool struct_ok = (a.rows() == b.rows()) && (a.nnz() == b.nnz());
    uint64_t col_mismatch = 0, val_diff = 0;
    double max_rel = 0.0;
    if (struct_ok) {
        std::vector<std::pair<index_type, value_type>> ra, rb;
        for (size_type i = 0; i < (size_type)a.rows(); ++i) {
            if (a.row_nnz(i) != b.row_nnz(i)) { struct_ok = false; break; }
            ra.clear(); rb.clear();
            ra.reserve(a.row_nnz(i)); rb.reserve(b.row_nnz(i));
            a.for_each_in_row(i, [&](index_type c, value_type v) {
                ra.push_back({c, v});
            });
            b.for_each_in_row(i, [&](index_type c, value_type v) {
                rb.push_back({c, v});
            });
            if (ra.size() != rb.size()) { struct_ok = false; break; }
            bool row_ok = true;
            for (size_t j = 0; j < ra.size(); ++j) {
                if (ra[j].first != rb[j].first) {
                    ++col_mismatch;
                    row_ok = false;
                    break;
                }
                double va = (double)ra[j].second, vb = (double)rb[j].second;
                double d = va - vb; if (d < 0) d = -d;
                double den = va < 0 ? -va : va;
                double rel = (den > 1e-30) ? d / den : d;
                if (rel > max_rel) max_rel = rel;
                if (rel > 1e-6) ++val_diff;
            }
            if (!row_ok) { struct_ok = false; break; }
        }
    }
    std::printf("FRAGABCHECK,%d,%d,%s,%s,struct=%s,nnz_a=%zu,nnz_b=%zu,"
                "col_mismatch=%llu,val_diff=%llu,max_rel=%.3e\n",
                r, s, tag, kind, struct_ok ? "ok" : "BAD",
                (size_t)a.nnz(), (size_t)b.nnz(),
                (unsigned long long)col_mismatch,
                (unsigned long long)val_diff, max_rel);
}

// ---- 2. charged normalization (C.1/C.2/C.3 repair for the frag arm) -------
// Returns false on the C19 abort (a duplicate group touches is_set=true).
// On true: *prep_ms holds the charged preparation time and `out` holds one
// PaddedAllocator per thread, each owning ONLY its band rows, (row,col)
// ascending with duplicates coalesced by SUM in record order -- the V4 rule
// for is_set-free groups (v2 contract, SparseMatrix:699-700; chain at 1087).
// Bucketing by owning thread reproduces v2's "thread id ascending" record
// order; stable_sort keeps emission order inside a group. is_set groups are
// REJECTED rather than chain-emulated: they do not occur in LHRR (MM/H emit
// is_set=false everywhere -- the probe prints that count as evidence).
template <class M>
bool normalize(const std::vector<typename M::PaddedAllocator>& alloc,
               const std::vector<std::pair<typename M::size_type,
                                           typename M::size_type>>& rng,
               std::vector<typename M::PaddedAllocator>* out,
               double* prep_ms) {
    using Triplet = typename M::Triplet;
    auto t0 = std::chrono::steady_clock::now();
    const size_t nb = rng.size();
    uint64_t total = 0;
    for (const auto& a : alloc)
        a.ptr->for_each([&](const Triplet&) { ++total; });
    std::vector<std::vector<Triplet>> bucket(nb);
    for (auto& v : bucket)
        v.reserve(total / (nb ? nb : 1) + 1024);
    for (const auto& a : alloc)
        a.ptr->for_each([&](const Triplet& x) {
            size_t d = 0;
            while (d + 1 < nb && x.row >= rng[d].second) ++d;
            bucket[d].push_back(x);
        });
    out->clear();
    out->resize(nb);   // default-constructs a PaddedAllocator per thread
    bool ok = true;
    for (size_t d = 0; ok && d < nb; ++d) {
        auto& v = bucket[d];
        std::stable_sort(v.begin(), v.end(),
                         [](const Triplet& l, const Triplet& r) {
                             return l.row != r.row ? l.row < r.row
                                                   : l.col < r.col;
                         });
        size_t w = 0, i = 0;   // in-place coalesce
        while (i < v.size()) {
            size_t j = i + 1;
            bool has_set = v[i].is_set;
            while (j < v.size() && v[j].row == v[i].row &&
                   v[j].col == v[i].col) {
                has_set = has_set || v[j].is_set;
                ++j;
            }
            if (j - i > 1) {
                if (has_set) { ok = false; break; }   // C19 backstop
                for (size_t k = i + 1; k < j; ++k)
                    v[i].value += v[k].value;         // V4 chain: sum
            }
            v[w] = v[i];
            ++w;
            i = j;
        }
        v.resize(w);
        for (const auto& t : v) *out->at(d).ptr->allocate() = t;
        v.clear();
        v.shrink_to_fit();  // keep the transient peak at ~live + out + one band
    }
    *prep_ms = ms_since(t0);
    return ok;
}

// ---- 3. the interleave + first-counterpart check --------------------------
// Called ONCE per natural build with the natural ms already measured by the
// caller (bench build_into / CSRMatrix ABHook). Emits:
//   FRAGPROBE -> [FRAGPREP|FRAGREJECT] -> natural sample -> N O N O N O
//   (both arms end with 3 samples) -> FRAGABCHECK(natural, FIRST counterpart).
// Measurement builds READ the live input allocators and write to scratch
// matrices; `nat` (the pipeline's real matrix) is never overwritten.
template <class M>
void measure(M& nat, std::vector<typename M::PaddedAllocator>& alloc,
             const std::vector<std::pair<typename M::size_type,
                                         typename M::size_type>>& rng,
             const char* tag, double nat_ms, bool natural_is_frag) {
    const int s = seq, r = rep;
    in_ = true;   // fence: any scratch fragment build must not re-enter
    // 1. contract probe on the live emitted base (counted, not asserted)
    Probe<M> p = probe<M>(alloc, rng);
    const char* verdict = !p.needs_norm() ? "ok"
        : (p.dupes && p.is_set_true ? "needs_norm(C19risk)" : "needs_norm");
    std::printf("FRAGPROBE,%d,%d,%s,emitted=%llu,band_viol=%llu,"
                "order_viol=%llu,dupes=%llu,is_set=%llu,verdict=%s\n",
                r, s, tag, (unsigned long long)p.emitted,
                (unsigned long long)p.band_viol,
                (unsigned long long)p.order_viol,
                (unsigned long long)p.dupes,
                (unsigned long long)p.is_set_true, verdict);
    // 2. charged normalization when the contract is missing (frag arm only;
    //    v2 accepts any base as emitted and never pays prep)
    std::vector<typename M::PaddedAllocator> norm_alloc;
    double prep_ms = 0.0;
    bool have_norm = false;
    if (p.needs_norm()) {
        have_norm = normalize<M>(alloc, rng, &norm_alloc, &prep_ms);
        if (have_norm) {
            std::printf("FRAGPREP,%d,%d,%s,prep_ms=%.3f,mode=norm\n",
                        r, s, tag, prep_ms);
        } else {
            std::printf("FRAGREJECT,%d,%d,%s,reason=dupes_and_is_set(C19)\n",
                        r, s, tag);
        }
    }
    // 3. interleaved samples over the identical base
    auto build_ms = [&](bool want_frag, auto& src, M& dst) {
        dst.reset();   // drop finalized_/old arrays; keeps rows_/cols_
        auto t0 = std::chrono::steady_clock::now();
        if (want_frag)
            M::build_from_ordered_fragments(dst, src, rng);
        else
            M::build_from_partial_results(
                dst, src, const_cast<std::vector<std::pair<
                    typename M::size_type, typename M::size_type>>&>(rng));
        return ms_since(t0);
    };
    auto emit = [&](const char* builder, const char* mode, double msx, int k) {
        std::printf("FRAGAB,%d,%d,%s,%s,%s,sample=%d,ms=%.3f\n",
                    r, s, tag, builder, mode, k, msx);
    };
    const char* nat_builder = natural_is_frag ? "frag" : "v2";
    const char* oth_builder = natural_is_frag ? "v2"   : "frag";
    const char* nat_mode = (natural_is_frag && have_norm) ? "norm"
                                                          : "direct";
    const char* oth_mode = (!natural_is_frag && have_norm) ? "norm"
                                                           : "direct";
    // Fragment arm possible: contract already held, or norm repaired it.
    const bool do_frag = !p.needs_norm() || have_norm;
    auto& frag_src = have_norm ? norm_alloc : alloc;
    // Scratch outputs: both builders REQUIRE rows preset and a non-finalized
    // matrix (they throw otherwise: SparseMatrix:544 / :934); rows_/cols_
    // come from the live natural matrix, and reset() (public, :1929) clears
    // the finalized flag + arrays between rounds while keeping rows_/cols_.
    M s_nat(nat.rows(), nat.cols()), s_oth(nat.rows(), nat.cols());
    int k = 0;
    emit(nat_builder, "direct", nat_ms, k++);   // the real natural build
    for (int round = 0; round < 3; ++round) {
        // counterpart first on round 0 so the check has its target
        if (natural_is_frag) {
            double msx = build_ms(false, alloc, s_oth);
            emit(oth_builder, "direct", msx, k++);
            if (round == 0) check<M>(nat, s_oth, r, s, tag, "frag_vs_v2");
        } else if (do_frag) {
            double msx = build_ms(true, frag_src, s_oth);
            emit(oth_builder, oth_mode, msx, k++);
            if (round == 0) check<M>(nat, s_oth, r, s, tag, "v2nat_vs_frag");
        }
        // natural-builder rebuilds (samples 2 and 3)
        if (round < 2) {
            auto& nsrc = natural_is_frag ? frag_src : alloc;
            double msx = build_ms(natural_is_frag, nsrc, s_nat);
            emit(nat_builder, nat_mode, msx, k++);
        }
    }
    in_ = false;
}

// ---- hooks (signatures match CSRMatrix::ABHook / bench build_into) --------
template <class M>
void after_partial(M& nat, std::vector<typename M::PaddedAllocator>& alloc,
                   const std::vector<std::pair<typename M::size_type,
                                               typename M::size_type>>& rng,
                   const char* tag, double nat_ms) {
    if (!on || in_) return;
    ++seq;   // counted natural build: MM=1, H=2 (W/ab excluded by the caller)
    measure<M>(nat, alloc, rng, tag, nat_ms, /*natural_is_frag=*/false);
}

template <class M>
void after_frag(M& nat, std::vector<typename M::PaddedAllocator>& alloc,
                const std::vector<std::pair<typename M::size_type,
                                            typename M::size_type>>& rng,
                double nat_ms) {
    if (!on || in_) return;
    ++seq;   // S=3, Srev=4, Stot=5, result=6 (natural LHRR build order)
    measure<M>(nat, alloc, rng, "(seq)", nat_ms, /*natural_is_frag=*/true);
}

}  // namespace phaseab

#endif  // PHASE_PROFILE
#endif  // PHASE_AB_HPP