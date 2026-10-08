// f1_formula_probe.cpp -- WHICH formula does the core actually implement?
//
// Two candidates for the F1 sim cell:
//   A (the legacy, RFE.cpp:180-193):  pow(sig(posqi), 2) * sig(posiq)
//   B (what I claimed in FINDINGS):  pow(sig(posiq), 2) * sig(posiq)
//
// The core's own sim() is pinned (ck_sim_dense == 0x8afb15dd0502c7cb), so
// comparing the core's actual cell against both candidates settles it without
// appealing to anything I wrote earlier.
#include "Methods/hpp/RFECore.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

static std::vector<int> gen_fixture(unsigned seed, int N, int L) {
  std::vector<int> rk((std::size_t)N * L);
  std::mt19937 rng(seed);
  std::vector<int> perm((std::size_t)N);
  std::iota(perm.begin(), perm.end(), 0);
  for (int i = 0; i < N; ++i) {
    std::shuffle(perm.begin(), perm.end(), rng);
    auto it = std::find(perm.begin(), perm.end(), i);
    std::swap(perm[0], *it);
    for (int j = 0; j < L; ++j) rk[(std::size_t)i * L + j] = perm[j];
  }
  return rk;
}

int main() {
  const int N = 200, L = 40, k = 10, t = 2, seed = 42;
  const double pa = 0.1;
  const int pk = k / 2;
  std::vector<int> rk = gen_fixture((unsigned)seed, N, L);

  auto sig = [&](int x) -> float {
    if (x == 1) return 1;
    float s = 1 / (1 + exp((-pa) * (x - pk)));
    return 1 - s;
  };
  auto getPosition = [&](int qImg, int img) -> int {
    for (int i = 0; i < L; ++i)
      if (rk[(std::size_t)L * qImg + i] == img) return i + 1;
    return L + 2;
  };

  // RFECore takes std::vector<int>& and MUTATES it (sort_ranked_lists rewrites
  // every row). Hand it a copy or the formulas below are evaluated against
  // cells that no longer exist -- which is exactly what happened on the
  // first run of this probe, and it answered NEITHER.
  std::vector<int> rk_core = rk;
  RFECore core(rk_core, (size_t)N, (size_t)L, k, t, pa);
  core.f1_rank_norm();
  std::printf("core ck_sim_dense=0x%016llx (pin 0x8afb15dd0502c7cb)\n",
              (unsigned long long)core.ck_sim_dense());

  int a_ok = 0, b_ok = 0, shown = 0;
  for (int q = 0; q < N; ++q) {
    for (int i = 0; i < L; ++i) {
      const int img = rk[(std::size_t)L * q + i];
      const int direct = i + 1;
      const int recip = getPosition(img, q);
      const float A = (float)(pow((double)sig(direct), 2.0) * (double)sig(recip));
      const float B = (float)(pow((double)sig(recip), 2.0) * (double)sig(recip));
      const float got = core.sim().get(q, img);
      if (got == A) ++a_ok;
      if (got == B) ++b_ok;
      if (shown < 4 && direct != recip) {
        std::printf("  q=%d i=%d img=%d direct=%d recip=%d | core=%.9g  A=%.9g  B=%.9g\n",
                    q, i, img, direct, recip, got, A, B);
        ++shown;
      }
    }
  }
  std::printf("CELLS matched by A (pow(sig(direct),2)*sig(recip)) : %d / %d\n", a_ok, N * L);
  std::printf("CELLS matched by B (pow(sig(recip),2)*sig(recip))   : %d / %d\n", b_ok, N * L);
  std::printf("VERDICT: the core implements %s\n",
              a_ok == N * L ? "A -- the legacy, verbatim" :
              b_ok == N * L ? "B" : "NEITHER");
  return a_ok == N * L ? 0 : 1;
}
