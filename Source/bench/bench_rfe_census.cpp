// bench_rfe_census.cpp -- Step 3 Stage 0: structural census of REAL rank lists,
// BEFORE any expensive run (the bench protocol's "touch-first": a census of
// minutes can refute the premise of a campaign of hours).
//
// It answers three questions, none of which the synthetic fixtures can:
//
//   A) SELF POSITION. The grValue > 0 theorem (FINDINGS.md) rests on self being
//      at position 0, because createEmb's pos1=0 term then contributes
//      rankKScore(1) = 1.0 to emb_pre[q][q]. The BFSTree dossier records that
//      real ALOI has self at 0 in only 71.448/72k rows -- so the theorem's
//      PREMISE may not hold on real data. Measured here per row, not assumed.
//   B) INPUT CONTRACT. Rows with fewer than L survivors after the id<N filter,
//      or without self, must be REFUSED, not silently filled.
//   C) rho(Gram). The decisive number. The Gram's support is
//         incElems[i] = union over {q : i in sorted_emb[q]} of sorted_emb[q]
//      and the legacy materialises it as a DENSE N^2 float matrix. This counts
//      each row's DISTINCT union size WITHOUT allocating the Gram, using a
//      per-row timestamp array instead of a sort -- so it runs at production N.
//
// (C) decides whether RFE is feasible at production scale at all, and therefore
// whether the union prune is an optimisation or a REQUIREMENT.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp Source/bench/bench_rfe_census.cpp -o /tmp/bench_rfe_census

#include "Methods/hpp/RFECore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The real cache is a raw int32 LE, row-major, stride L dump. N is DERIVED from
// the file size rather than passed in, so a wrong N cannot silently shift the
// stride and produce plausible-looking garbage.
bool load_raw(const char* path, int L, std::vector<int>& rk, long long& N_out,
              long long& bytes) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::printf("FATAL: cannot open %s\n", path);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  const long long cells = bytes / 4;
  if (cells % L != 0) {
    std::printf("FATAL: file cells (%lld) not a multiple of L=%d -- stride mismatch\n",
                cells, L);
    std::fclose(f);
    return false;
  }
  N_out = cells / L;
  rk.resize((std::size_t)cells);
  if (std::fread(rk.data(), 1, (std::size_t)bytes, f) != (std::size_t)bytes) {
    std::printf("FATAL: short read\n");
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  return true;
}

// Peak RSS of THIS process, from /proc/self/status VmHWM. Reported because a Gram
// payload is not a memory claim: the peak also carries the position index
// (~230 MB), emb and remb, F6's raw candidate vectors and sim_. "233x less than
// the legacy" is meaningless unless BOTH peaks are on the table.
long vm_hwm_kb() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "VmHWM:", 6) == 0) { kb = std::atol(line + 6); break; }
  }
  std::fclose(f);
  return kb;
}

double now_s() {
  using clk = std::chrono::steady_clock;
  return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::printf(
        "uso: bench_rfe_census <bin> <L> <k> <t> <pa> [Nlimit] [rho_N]\n"
        "  Nlimit : self census over the first Nlimit rows (0 = all)\n"
        "  rho_N  : sub-sample to this N for the rho(Gram) census (0 = Nlimit)\n");
    return 2;
  }
  const char* path = argv[1];
  const int L = std::atoi(argv[2]);
  const int k = std::atoi(argv[3]);
  const int t = std::atoi(argv[4]);
  const double pa = std::atof(argv[5]);
  const long long Nlimit = (argc > 6) ? std::atoll(argv[6]) : 0;
  const long long rho_N = (argc > 7) ? std::atoll(argv[7]) : Nlimit;

  std::vector<int> rk;
  long long N = 0, bytes = 0;
  if (!load_raw(path, L, rk, N, bytes)) return 2;
  std::printf("RKCENSUS file=%s bytes=%lld cells=%lld L=%d N=%lld k=%d t=%d pa=%g\n",
              path, bytes, bytes / 4, L, N, k, t, pa);

  // ---------------------------------------------------------------------
  // A) SELF POSITION census over every row. Cheap: one pass, no allocation.
  // ---------------------------------------------------------------------
  {
    long long at0 = 0, inL = 0, inall = 0, oob = 0;
    const long long Nc = Nlimit ? std::min(Nlimit, N) : N;
    for (long long q = 0; q < Nc; ++q) {
      const int* row = rk.data() + (std::size_t)q * (std::size_t)L;
      bool a0 = false, iL = false, ia = false;
      for (int j = 0; j < L; ++j) {
        const int v = row[j];
        if (v < 0 || (long long)v >= N) { ++oob; continue; }
        if (v == (int)q) {
          ia = true;
          iL = true;
          if (j == 0) a0 = true;
        }
      }
      at0 += a0; inL += iL; inall += ia;
    }
    std::printf("RKSELF rows=%lld self_at_0=%lld (%.3f%%) self_in_L=%lld (%.3f%%) "
                "self_in_all=%lld (%.3f%%) out_of_range_ids=%lld\n",
                Nc, at0, 100.0 * (double)at0 / (double)Nc,
                inL, 100.0 * (double)inL / (double)Nc,
                inall, 100.0 * (double)inall / (double)Nc, oob);
    std::printf("  VERDICT grValue-theorem premise (self at 0): %s\n",
                at0 == Nc ? "HOLDS on every row"
                          : "VIOLATED -- FINDINGS.md's theorem premise needs restating");
  }

  // ---------------------------------------------------------------------
  // B+C) contract + rho(Gram) on a sub-sample, WITHOUT materialising the Gram.
  // ---------------------------------------------------------------------
  {
    const long long Nc = rho_N ? std::min(rho_N, N) : N;
    if (Nc > N) { std::printf("FATAL: rho_N > N\n"); return 2; }

    // Keep row q's first L ids that are < Nc, in order. Rows left short, or
    // without self, are REFUSED -- input contract, not a fill.
    std::vector<int> sub((std::size_t)Nc * (std::size_t)L, 0);
    long long dropped_short = 0, dropped_noself = 0, first_bad = -1;
    for (long long q = 0; q < Nc; ++q) {
      const int* src = rk.data() + (std::size_t)q * (std::size_t)L;
      int* dst = sub.data() + (std::size_t)q * (std::size_t)L;
      int w = 0;
      bool has_self = false;
      for (int j = 0; j < L && w < L; ++j) {
        const int v = src[j];
        if (v < 0 || (long long)v >= Nc) continue;   // id filter
        dst[w++] = v;
        if (v == (int)q) has_self = true;
      }
      if (w < L) { ++dropped_short; if (first_bad < 0) first_bad = q; continue; }
      if (!has_self) { ++dropped_noself; if (first_bad < 0) first_bad = q; continue; }
    }
    std::printf("RKCONTRACT rows=%lld kept=%lld dropped_short=%lld dropped_noself=%lld"
                " first_bad=%lld\n",
                Nc, Nc - dropped_short - dropped_noself, dropped_short,
                dropped_noself, first_bad);

    // CONTRACT REFUSAL, not a fill (bench README pitfall 3: a guard is an
    // assertion, never a min()). Filtering ids < Nc from a row of L ids drawn
    // from [0,N) leaves about L*(Nc/N) survivors -- so shrinking Nc while
    // keeping L CANNOT preserve the row width, and the honest answer is to
    // REFUSE. That refusal is itself the Stage 2 feasibility measurement.
    if (dropped_short || dropped_noself) {
      std::printf("RKCONTRACT_REFUSED: %lld/%lld rows violate the contract at"
                  " N=%lld L=%d (expected survivors ~ L*(Nsub/Nfull) = %.1f"
                  " of %d) -- refusing to fill\n",
                  dropped_short + dropped_noself, Nc, Nc, L,
                  (double)L * (double)Nc / (double)N, L);
      return 5;
    }

    const bool prof = getenv("RFE_F1PROF") != nullptr;
    const double t0 = now_s();
    RFECore core(sub, (std::size_t)Nc, (std::size_t)L, k, t, pa);
    core.set_f1_profile(prof);
    core.set_f6_profile(prof);
    core.set_run_profile(prof);
    // X9: ONE definition of the sequence. The census calls run() and reads the
    // state afterwards; it never spells the phase list itself. The earlier
    // explicit f1..f7b here was a copy, and it is the copy that measured a
    // pipeline with no t-loop and produced every superseded number in FINDINGS.
    core.run();
    const double t_f1 = now_s() - t0;
    // Per-phase times now come from the RunProfile, which lives INSIDE run().
    const double t_f24 = 0.0, t_f5 = 0.0;

    const auto& se = core.sorted_emb();
    long long tot_keys = 0;
    for (long long q = 0; q < Nc; ++q) tot_keys += (long long)se[(std::size_t)q].size();
    std::printf("RKPIPE N=%lld L=%d k=%d t=%d | sim_nnz=%zu emb_nnz=%zu emb_zeros=%zu"
                " sorted_emb_keys=%lld mean_row=%.1f | T_f1=%.2fs T_f24=%.2fs"
                " T_f5=%.2fs\n",
                Nc, L, k, t, core.nnz_sim(), core.nnz_emb(), core.emb_exact_zeros(),
                tot_keys, (double)tot_keys / (double)Nc, t_f1, t_f24, t_f5);

    // Inverted index: for each element i, the rows whose sorted_emb contains i.
    std::vector<std::vector<std::uint32_t>> inv((std::size_t)Nc);
    for (long long q = 0; q < Nc; ++q)
      for (const int i : se[(std::size_t)q]) inv[(std::size_t)i].push_back((std::uint32_t)q);

    // Distinct union size per row via a timestamp array -- no per-row sort, so
    // this stays linear in the candidate count and runs at production N.
    std::vector<std::int64_t> mark((std::size_t)Nc, -1);
    long long nnz_est = 0, row_max = 0, nnz_pruned = 0;
    const double t4 = now_s();
    for (long long i = 0; i < Nc; ++i) {
      long long c = 0;
      for (const std::uint32_t q : inv[(std::size_t)i])
        for (const int j : se[q]) {
          if (mark[(std::size_t)j] != i) { mark[(std::size_t)j] = i; ++c; }
        }
      nnz_est += c;
      // What the union prune would actually KEEP for this row: min(L, |row|).
      // NOT N*L -- the average row is smaller than L at real scale, so assuming
      // a full L per row OVERSTATES the pruned size and inverts the conclusion.
      nnz_pruned += (c < (long long)L) ? c : (long long)L;
      if (c > row_max) row_max = c;
    }
    const double t_rho = now_s() - t4;

    const double cells_dense = (double)Nc * (double)Nc;
    std::printf("RKGRAM N=%lld nnz_est=%lld row_max=%lld mean_row=%.1f rho_gram=%.6f"
                " (T_census=%.2fs)\n",
                Nc, nnz_est, row_max, (double)nnz_est / (double)Nc,
                (double)nnz_est / cells_dense, t_rho);
    std::printf("RKMEM  legacy dense = %.2f GB | CSR gram (unpruned) = %.3f GB"
                " | CSR gram (union-pruned to L) = %.3f GB [prune would SAVE %.1f%%]\n",
                cells_dense * 4.0 / 1e9, (double)nnz_est * 8.0 / 1e9,
                (double)nnz_pruned * 8.0 / 1e9,
                100.0 * (1.0 - (double)nnz_pruned / (double)nnz_est));
    std::printf("RKTOTAL %.2fs\n", now_s() - t0);

    // ---------------------------------------------------------------------
    // Full pipeline at real scale: F6 (Gram) + F7b (tail). Enabled because the
    // census above shows the Gram is only ~0.09 GB at N=72000, so it FITS --
    // unlike the legacy, which needs N^2*4 = 20.7 GB for the same thing.
    // ---------------------------------------------------------------------
    if (getenv("RFE_FULL")) {
      // run() has already done F6 and F7b. This block only READS; calling f6
      // again would rebuild the Gram on top of the post-t-loop state and report
      // a different object than the one the pipeline actually produced.
      const auto& rp = core.run_profile();
      const double t_f6 = rp.t_f6, t_f7b = rp.t_f7b;
      std::printf("RKF6 N=%lld gram_nnz=%zu gram_zeros=%zu (%.2f%% of nnz)"
                  " incElemsL_rows=%zu ck_gram=0x%016llx | T_f6=%.2fs\n",
                  Nc, core.nnz_gram(), core.gram_exact_zeros(),
                  core.nnz_gram() ? 100.0 * (double)core.gram_exact_zeros() /
                                       (double)core.nnz_gram() : 0.0,
                  core.inc_elems_l().size(),
                  (unsigned long long)core.ck_gram(), t_f6);
      if (rp.enabled) {
        std::printf("RKRUNPROF N=%lld L=%d k=%d t=%d threads=%d  (t-loop INCLUDED)\n",
                    Nc, L, k, t, rp.threads);
        std::printf("  t_total=%.3f\n", rp.t_total);
        const double tot = rp.t_total > 0 ? rp.t_total : 1.0;
        struct P { const char* n; double v; };
        const P ph[] = {
          {"f1_rank_norm      ", rp.t_f1},   {"embRR f2 (t-loop)", rp.t_f2t},
          {"embRR f3 (t-loop)", rp.t_f3t},  {"embRR f4 (t-loop)", rp.t_f4t},
          {"f7a rerank (t-loop)", rp.t_f7at}, {"embRR FINAL      ", rp.t_embrr},
          {"f5 sortEmb       ", rp.t_f5sort},{"f5 effectEst     ", rp.t_f5eff},
          {"f6 Gram+incElems", rp.t_f6},    {"f7b CPRR tail    ", rp.t_f7b},
        };
        for (const P& e : ph)
          std::printf("    %s %8.3f s (%5.1f%%)\n", e.n, e.v, 100.0 * e.v / tot);
        std::printf("  per t-loop iteration (counts computed OUTSIDE the loop):\n");
        for (std::size_t it = 0; it < rp.emb_nnz_it.size(); ++it)
          std::printf("    it%zu emb_nnz=%lld emb_zeros=%lld f7a_row_visits=%lld"
                      " f7a_merges=%lld\n", it, rp.emb_nnz_it[it],
                      rp.emb_zeros_it[it], rp.f7a_visits_it[it],
                      (long long)Nc * (long long)L);
        std::printf("    final emb_nnz=%lld  f7a_merges_total=%lld\n",
                    rp.emb_nnz_final, rp.f7a_merges_total);
        std::printf("  PEAK VmHWM by boundary (clear_refs=5 before each phase):\n");
        {
          const struct { const char* n; double v; } pk[] = {
            {"after f1      ", rp.pk_f1},   {"after t-loop  ", rp.pk_tloop},
            {"after embRR   ", rp.pk_embrr},{"after f5      ", rp.pk_f5},
            {"after f6 Gram ", rp.pk_f6},   {"after f7b     ", rp.pk_f7b},
          };
          for (int z = 0; z < 6; ++z)
            std::printf("    %-16s %8.1f MB\n", pk[z].n, pk[z].v / 1024.0);
        }

      }
      const auto& f1 = core.f1_profile();
      if (f1.enabled) {
        std::printf("RKF1PROF N=%lld L=%d k=%d threads=%d\n", Nc, L, k, f1.threads);
        std::printf("  t_total=%.3f (wall)\n", f1.t_total);
        std::printf("    t_index    =%.3f (%.1f%%) SERIAL build_position_index\n",
                    f1.t_index, 100.0 * f1.t_index / f1.t_total);
        std::printf("    t_loop     =%.3f (%.1f%%) parallel region wall\n",
                    f1.t_loop, 100.0 * f1.t_loop / f1.t_total);
        std::printf("    t_cells_max=%.3f (%.1f%%) CRITICAL PATH inside the loop\n",
                    f1.t_cells_max, 100.0 * f1.t_cells_max / f1.t_total);
        std::printf("    t_build    =%.3f (%.1f%%) SERIAL build_from_partial_results\n",
                    f1.t_build, 100.0 * f1.t_build / f1.t_total);
        std::printf("    t_sortrows =%.3f (%.1f%%) sort_ranked_lists\n",
                    f1.t_sortrows, 100.0 * f1.t_sortrows / f1.t_total);
        std::printf("  cells=%lld  position_of absent(l+2)=%lld (%.4f%%)\n",
                    f1.cells, f1.absent,
                    f1.cells ? 100.0 * (double)f1.absent / (double)f1.cells : 0.0);
      }
      const auto& fp = core.f6_profile();
      if (fp.enabled) {
        std::printf("RKF6PROF N=%lld threads=%d\n", Nc, fp.threads);
        std::printf("  t_total=%.3f (wall)  t_inv=%.3f (serial)  t_build=%.3f (serial)\n",
                    fp.t_total, fp.t_inv, fp.t_build);
        // Two denominators, on purpose. SUM is CPU work -> share of the available
        // thread-seconds is sum/(t_total*threads). MAX is the critical path ->
        // its share of the wall clock is max/t_total, and IT is what a parallel
        // wall time can actually be reduced by.
        const double cap = fp.t_total * (double)fp.threads;
        std::printf("  phase        sum(thread-s)  %%capacity   max(critical)  %%wall\n");
        const double s[4] = {fp.t_disc, fp.t_sort, fp.t_acc, fp.t_emit};
        const double m[4] = {fp.m_disc, fp.m_sort, fp.m_acc, fp.m_emit};
        const char* nm[4] = {"t_disc", "t_sort", "t_acc ", "t_emit"};
        const char* de[4] = {"pass1 discovery", "finalize_candidates",
                             "pass2 ORDERED acc", "triplet emission"};
        for (int k = 0; k < 4; ++k)
          std::printf("    %s %10.3f %10.1f%% %12.3f %9.1f%%  %s\n", nm[k], s[k],
                      100.0 * s[k] / (cap > 0 ? cap : 1.0), m[k],
                      100.0 * m[k] / (fp.t_total > 0 ? fp.t_total : 1.0), de[k]);
        std::printf("  work: cand_raw=%lld cand_uniq=%lld inner_iters=%lld\n",
                    fp.cand_raw, fp.cand_uniq, fp.inner_iters);
        std::printf("  raw/uniq=%.2f  iters/uniq=%.2f  inv_min=%lld inv_max=%lld"
                    " (imbalance %.1fx)\n",
                    (double)fp.cand_raw / (double)fp.cand_uniq,
                    (double)fp.inner_iters / (double)fp.cand_uniq,
                    fp.inv_min, fp.inv_max,
                    (double)fp.inv_max / (double)(fp.inv_min ? fp.inv_min : 1));
        // The decisive ratio: insertion sort is O(k^2) in the RAW candidate count
        // while the binary searches are O(log uniq). If raw/uniq is large, the
        // sort dominates and the strictness is NOT the bottleneck.
        std::printf("  est insert-sort comparisons ~ cand_raw^2/(2*n) = %.3g"
                    " vs binary-search steps ~ inner_iters*log2(uniq) = %.3g\n",
                    (double)fp.cand_raw * (double)fp.cand_raw /
                        (2.0 * (double)Nc),
                    (double)fp.inner_iters *
                        (log((double)fp.cand_uniq / (double)Nc) /
                         log(2.0)));
      }
      // F7b already ran inside run(); nothing to re-invoke here.
      std::printf("RKF7B N=%lld ck_rk=0x%016llx | T_f7b=%.2fs\n", Nc,
                  (unsigned long long)core.ck_rk(), t_f7b);
      std::printf("RKPIPEFULL total=%.2fs (T_f1=%.2f T_f24=%.2f T_f5=%.2f"
                  " T_f6=%.2f T_f7b=%.2f) VmHWM=%ldMB\n",
                  now_s() - t0, t_f1, t_f24, t_f5, t_f6, t_f7b, vm_hwm_kb() / 1024);
    }
  }
  return 0;
}