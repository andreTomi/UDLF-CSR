// f5_tiebreak_control.cpp -- Step 3 / Block 2: the F5 tie-break NEGATIVE CONTROL.
//
// A tied `ee` population is a COUNT. Exposure is a different question: the
// tie-break only reaches the Gram if two rows tied in `ee` contribute NON-ZERO
// values to the same cell in different orders. 0.0f + x == x + 0.0f exactly, so
// a field of exact zeros cannot expose it at all.
//
// So: run the whole pipeline twice on the SAME real input, once with the
// legacy's unstable std::sort and once with std::stable_sort, and see whether
// anything downstream moves. If nothing moves, ties exist and exposure is zero,
// and the honest claim is "NOT DEMONSTRATED" -- not "covered".
#include "Methods/hpp/RFECore.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<int> load(const char* path, int N, int L) {
  std::vector<int> rk;
  rk.reserve((std::size_t)N * L);
  FILE* f = fopen(path, "r");
  if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
  for (int i = 0; i < N * L; ++i) { int v; if (fscanf(f, "%d", &v) != 1) { std::printf("short read at %d\n", i); std::exit(5);} rk.push_back(v); }
  int junk; if (fscanf(f, "%d", &junk) == 1) { std::printf("extra columns: file wider than N=%d L=%d\n", N, L); std::exit(5); }
  fclose(f);
  return rk;
}

static void run_all(std::vector<int> rk, int N, int L, int k, int t, double pa,
                    bool stable, RFECore** out) {
  RFECore* c = new RFECore(rk, (std::size_t)N, (std::size_t)L, k, t, pa);
  c->set_f5_sort_probe(stable);
  c->f1_rank_norm(); c->f2_create_emb(); c->f3_create_reverse_emb();
  c->f4_combine_reverse_emb(); c->f5_sort_emb(); c->f5_effect_estimation_emb();
  c->f6_gram_and_inc_elems(); c->f7b_rerank_cprr_tail();
  *out = c;
}

int main(int argc, char** argv) {
  const char* path = argv[1];
  const int N = atoi(argv[2]), L = atoi(argv[3]), k = atoi(argv[4]), t = 2;
  const double pa = 0.1;
  std::vector<int> rk0 = load(path, N, L);

  RFECore *a = nullptr, *b = nullptr;
  run_all(rk0, N, L, k, t, pa, false, &a);
  run_all(rk0, N, L, k, t, pa, true, &b);

  // tie population shape
  long groups = 0, max_group = 0;
  {
    std::vector<float> v = a->ee();
    std::sort(v.begin(), v.end());
    std::size_t i = 0;
    while (i < v.size()) {
      std::size_t j = i;
      while (j < v.size() && v[j] == v[i]) ++j;
      const long g = (long)(j - i);
      if (g > 1) { ++groups; if (g > max_group) max_group = g; }
      i = j;
    }
  }

  // sorted_ee() is a FLAT vector: the tag_sort ORDER as a permutation of
  // 0..N-1, not a vector of rows. The positions that matter are its slots.
  long see_diff = 0;
  const long m = (long)a->sorted_ee().size();
  for (long i = 0; i < m; ++i)
    if (a->sorted_ee()[(std::size_t)i] != b->sorted_ee()[(std::size_t)i]) ++see_diff;

  const bool g_same = a->ck_gram() == b->ck_gram();
  const bool r_same = a->ck_rk() == b->ck_rk();

  std::printf("F5CTRL file=%s N=%d L=%d k=%d t=%d pa=%g\n", path, N, L, k, t, pa);
  std::printf("  tie groups=%ld  max group size=%ld\n", groups, max_group);
  std::printf("  sorted_ee positions differing (sort vs stable_sort) = %ld\n", see_diff);
  std::printf("  ck_gram sort=0x%016llx stable=0x%016llx  %s\n",
              (unsigned long long)a->ck_gram(), (unsigned long long)b->ck_gram(),
              g_same ? "IDENTICAL" : "DIFFERENT");
  std::printf("  ck_rk    sort=0x%016llx stable=0x%016llx  %s\n",
              (unsigned long long)a->ck_rk(), (unsigned long long)b->ck_rk(),
              r_same ? "IDENTICAL" : "DIFFERENT");
  std::printf("  VERDICT: tie-break %s on this dataset\n",
              g_same ? "IS NOT EXPOSED (exposure zero despite ties)"
                     : "IS EXPOSED (load-bearing)");
  return 0;
}
