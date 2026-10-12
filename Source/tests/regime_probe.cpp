// regime_probe.cpp -- Fase 0-B: measure the legacy oracle under THREE build
// regimes and record ck_rk (bits) so "bit-identical to legacy" is unambiguous.
//
// WHY. The acceptance criterion of the fast-mode project is equivalence to the
// legacy. But "the legacy" is only defined once its build is fixed. This repo
// ships three distinct regimes, and the original UDLF (UDLF-master) a fourth:
//
//   R1  master   : -std=gnu++14 -O3                 (UDLF-master/Makefile; the
//                                                     binary real users run;
//                                                     NO fast-math, NO -fopenmp)
//   R2  strict   : -O2 -ffp-contract=off -fopenmp   (this repo's udlf-strict /
//                                                     every parity gate)
//   R3  product  : <CFLAGS> with -march=native -mavx2 -mfma -ffast-math
//                                                     -funsafe-math-optimizations
//                                                     -ffinite-math-only -O3 -DNDEBUG
//
// This driver links ONLY the verbatim oracle (rfe_ref_verbatim.hpp) and uses NO
// OpenMP, so it compiles unchanged under all three flag sets -- including R1,
// which has no -fopenmp at all. It prints one greppable line:
//
//   REGIME ck_rk=0x<hex> wall_s=<s>
//
// and writes the final rank list as a raw u32 N*L dump (the sink rfe_quality.py
// --bin reads), so MAP can be computed per regime without recompiling.
//
// Usage:
//   regime_probe <rk_text> <N> <L> <k> <t> <pa> <out.u32> [--cc]
//
// Build is done by run_regime_probe.sh, which extracts CFLAGS from the Makefile
// (never retypes them) and drives all three regimes on the same fixture.

#include "rfe_ref_verbatim.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Text loader: identical REFUSE-never-pad contract as the other drivers (short
// read or extra token => exit 5). Fixture pins self at index 0.
std::vector<int> load_rk_text(const char* path, int N, int L) {
  std::vector<int> rk;
  rk.reserve((std::size_t)N * (std::size_t)L);
  std::FILE* f = std::fopen(path, "r");
  if (!f) { std::printf("FATAL: cannot open %s\n", path); std::exit(2); }
  for (int q = 0; q < N; ++q) {
    for (int j = 0; j < L; ++j) {
      int v = -1;
      if (std::fscanf(f, "%d", &v) != 1) {
        std::printf("RKLOAD_REFUSED: short read at row=%d col=%d\n", q, j);
        std::fclose(f); std::exit(5);
      }
      rk.push_back(v);
    }
    int c;
    for (;;) {
      c = std::fgetc(f);
      if (c == ' ' || c == '\t' || c == '\r') continue;
      break;
    }
    if (c != '\n' && c != EOF) {
      std::printf("RKLOAD_REFUSED: extra token at row=%d\n", q);
      std::fclose(f); std::exit(5);
    }
  }
  std::fclose(f);
  return rk;
}

double now_s() {
  using clk = std::chrono::steady_clock;
  return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 8) {
    std::printf("uso: regime_probe <rk_text> <N> <L> <k> <t> <pa> <out.u32> [--cc]\n");
    return 2;
  }
  const char* rkpath = argv[1];
  const int N = std::atoi(argv[2]);
  const int L = std::atoi(argv[3]);
  const int k = std::atoi(argv[4]);
  const int t = std::atoi(argv[5]);
  const double pa = std::atof(argv[6]);
  const char* outpath = argv[7];
  bool cc = false;
  for (int a = 8; a < argc; ++a) {
    if (std::strcmp(argv[a], "--cc") == 0) cc = true;
    else { std::printf("FATAL: unknown arg %s\n", argv[a]); return 2; }
  }

  std::vector<int> rk = load_rk_text(rkpath, N, L);
  std::printf("REGIME arm=legacy N=%d L=%d k=%d t=%d pa=%g cc=%d\n",
              N, L, k, t, pa, cc ? 1 : 0);

  const double t0 = now_s();
  RFERef ref(N, L, k, t, pa);
  ref.load_rk(rk);
  ref.run();                 // rankNorm -> t x embRR -> cartesian product (F1..F7b)
  if (cc) ref.exec_rr_ccs(); // legacy execRRCCs (F8g/F8h tail)
  rk = ref.rk();
  const double wall = now_s() - t0;

  std::FILE* f = std::fopen(outpath, "wb");
  if (!f) { std::printf("FATAL: cannot open %s for write\n", outpath); return 2; }
  const std::size_t bytes = rk.size() * sizeof(int);
  const bool ok = std::fwrite(rk.data(), 1, bytes, f) == bytes;
  std::fclose(f);
  if (!ok) return 2;

  std::printf("REGIME ck_rk=0x%016llx wall_s=%.3f\n",
              (unsigned long long)ref.ck_rk(), wall);
  std::printf("REGIME wrote=%s cells=%zu\n", outpath, rk.size());
  return 0;
}
