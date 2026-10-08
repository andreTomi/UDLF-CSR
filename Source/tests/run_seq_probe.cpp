// run_seq_probe.cpp -- Fase 1: what does run() actually produce on real data?
//
// The k sweep recorded ck_gram, Gram nnz, rho and exact-zeros from the F6
// section, which is driven to f6 on a PRE-t-loop state. ck_rk came from the
// E2E, which calls run(). Those two cannot both describe the same pipeline if
// ck_gram is L-invariant (it was identical at L=400 and L=1000) while ck_rk is
// not. This probe settles it: one run() call, everything reported from the same
// object.
#include "Methods/hpp/RFECore.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::vector<int> load(const char* path, int N, int L) {
  std::vector<int> rk; rk.reserve((std::size_t)N * L);
  FILE* f = fopen(path, "r");
  if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
  for (int i = 0; i < N * L; ++i) { int v; if (fscanf(f, "%d", &v) != 1) { std::printf("short read %d\n", i); std::exit(5);} rk.push_back(v); }
  int j; if (fscanf(f, "%d", &j) == 1) { std::printf("file wider than N=%d L=%d\n", N, L); std::exit(5); }
  fclose(f); return rk;
}

int main(int argc, char** argv) {
  const char* path = argv[1];
  const int N = atoi(argv[2]), L = atoi(argv[3]), k = atoi(argv[4]), t = atoi(argv[5]);
  const double pa = argc > 6 ? atof(argv[6]) : 0.1;
  std::vector<int> rk = load(path, N, L);
  RFECore core(rk, (std::size_t)N, (std::size_t)L, k, t, pa);
  // "short" mode runs the pre-t-loop sequence on purpose, so the two pipelines
  // can be told apart on the SAME data. It exists to be compared against, never
  // to be shipped -- run() is the only entry point the product uses (X9).
  const bool shortseq = (argc > 7 && std::string(argv[7]) == "short");
  if (shortseq) {
    core.f1_rank_norm();
    core.f2_create_emb(); core.f3_create_reverse_emb(); core.f4_combine_reverse_emb();
    core.f5_sort_emb(); core.f5_effect_estimation_emb();
    core.f6_gram_and_inc_elems(); core.f7b_rerank_cprr_tail();
    std::printf("RUNSEQ file=%s N=%d L=%d k=%d t=%d pa=%g  [SHORT: no f7a]\n",
                path, N, L, k, t, pa);
    std::printf("  ck_rk   =0x%016llx   ck_gram=0x%016llx   gram_nnz=%zu rho=%.6f\n",
                (unsigned long long)core.ck_rk(), (unsigned long long)core.ck_gram(),
                core.nnz_gram(),
                (double)core.nnz_gram() / ((double)N * (double)N));
    return 0;
  }
  core.set_run_profile(true);
  core.set_f1_profile(true);
  core.set_f6_profile(true);
  core.run();
  const RFECore::RunProfile& rp = core.run_profile();
  std::printf("RUNPROF threads=%d  t_total=%.3f\n", rp.threads, rp.t_total);
  {
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
      std::printf("  %s %8.3f s (%5.1f%%)\n", e.n, e.v, 100.0 * e.v / tot);
    for (std::size_t it = 0; it < rp.emb_nnz_it.size(); ++it)
      std::printf("  it%zu emb_nnz=%lld emb_zeros=%lld f7a_row_visits=%lld"
                  " f7a_merges=%lld  visits/merge=%.2f\n", it,
                  rp.emb_nnz_it[it], rp.emb_zeros_it[it], rp.f7a_visits_it[it],
                  (long long)N * (long long)L,
                  rp.f7a_visits_it[it] ? (double)rp.f7a_visits_it[it] /
                                          ((double)N * (double)L) : 0.0);
    std::printf("  final emb_nnz=%lld f7a_merges_total=%lld\n",
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
  const double rho = (double)core.nnz_gram() / ((double)N * (double)N);
  std::printf("RUNSEQ file=%s N=%d L=%d k=%d t=%d pa=%g\n", path, N, L, k, t, pa);
  std::printf("  ck_rk   =0x%016llx   (E2E / faithful)\n", (unsigned long long)core.ck_rk());
  std::printf("  ck_gram =0x%016llx   (FINAL, after run())\n", (unsigned long long)core.ck_gram());
  std::printf("  gram_nnz=%zu  rho=%.6f  exact_zeros=%zu (%.2f%%)\n",
              core.nnz_gram(), rho, core.gram_exact_zeros(),
              core.nnz_gram() ? 100.0 * (double)core.gram_exact_zeros() / (double)core.nnz_gram() : 0.0);
  std::printf("  emb_nnz=%zu  emb_exact_zeros=%zu (%.2f%%)  [FINAL embRR, feeds F5/F6]\n",
              core.nnz_emb(), core.emb_exact_zeros(),
              core.nnz_emb() ? 100.0 * (double)core.emb_exact_zeros() / (double)core.nnz_emb() : 0.0);
  return 0;
}
