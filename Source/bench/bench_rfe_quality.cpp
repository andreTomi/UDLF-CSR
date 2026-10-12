// bench_rfe_quality.cpp -- Fase 0 harness driver: ONE PROCESS PER ARM.
//
// Arms (argv[1]):
//   input   -- no computation; dumps the input ranking as-is (arm (a))
//   legacy  -- RFERef (rfe_ref_verbatim.hpp), the bit-exact legacy oracle (b)
//   exact   -- RFECore (Source/src/Methods/hpp/RFECore.hpp), arm (c)
//   fast    -- RFEFastCore (Fase 2+; refuses to build/run until it exists)
//
// Usage:
//   bench_rfe_quality <arm> <rk_text|bin> <N> <L> <k> <t> <pa> <out.u32>
//                     [--bin] [--cc] [--th_cc X] [--threads T]
//   --bin     : input file is a raw u32 N*L dump instead of text
//   --cc      : run with connected components (legacy exec_rr_ccs / core
//               performCCs=true; requires RFE_ALLOW_CC=1 for the core arm --
//               the harness sets nothing, the CALLER exports it)
//   --threads : omp_set_num_threads(T) before the run (determinism protocol)
//
// Output contract (stdout, greppable):
//   RFEQ arm=<arm> N=<N> L=<L> k=<k> t=<t> pa=<pa> cc=<0|1>
//   RFEQ wall_s=<s> VmHWM_kb=<kb> ck_rk=0x<hex>
//   RFEQ wrote=<path> bytes=<bytes>
//
// The final rank list is written as RAW u32 N*L little-endian (the same sink
// pk_eval.py --bin / rfe_quality.py --bin read). Labels/classes are NEVER
// read here: they live in the evaluator only.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp -I Source/tests
//       Source/bench/bench_rfe_quality.cpp -o /tmp/bench_rfe_quality

#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

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

std::uint64_t fnv1a(const void* data, std::size_t bytes) {
  const auto* b = static_cast<const std::uint8_t*>(data);
  std::uint64_t h = 1469598103934665603ULL;  // same basis as RFERef::ck_rk
  for (std::size_t i = 0; i < bytes; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

// Text loader: same REFUSE-never-pad contract as test_rfe_parity's load_rk_file
// (short read or extra token => exit 5).
std::vector<int> load_rk_text(const char* path, int N, int L) {
  std::vector<int> rk;
  rk.reserve((std::size_t)N * (std::size_t)L);
  std::FILE* f = std::fopen(path, "r");
  if (!f) { std::printf("FATAL: cannot open %s\n", path); std::exit(2); }
  for (int q = 0; q < N; ++q) {
    for (int j = 0; j < L; ++j) {
      int v = -1;
      if (std::fscanf(f, "%d", &v) != 1) {
        std::printf("RKLOAD_REFUSED: short read at row=%d col=%d (want N=%d L=%d)\n",
                    q, j, N, L);
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
      std::printf("RKLOAD_REFUSED: extra token at row=%d -- more than L=%d cols\n",
                  q, L);
      std::fclose(f); std::exit(5);
    }
  }
  std::fclose(f);
  return rk;
}

std::vector<int> load_rk_bin(const char* path, int N, int L) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) { std::printf("FATAL: cannot open %s\n", path); std::exit(2); }
  std::fseek(f, 0, SEEK_END);
  const long long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  const long long want = 4LL * N * L;
  if (bytes != want) {
    std::printf("RKLOAD_REFUSED: %s has %lld bytes, want %lld (N=%d L=%d)\n",
                path, bytes, want, N, L);
    std::fclose(f); std::exit(5);
  }
  std::vector<int> rk((std::size_t)N * (std::size_t)L);
  if (std::fread(rk.data(), 1, (std::size_t)want, f) != (std::size_t)want) {
    std::printf("FATAL: short read\n"); std::fclose(f); std::exit(2);
  }
  std::fclose(f);
  return rk;
}

bool write_rk_bin(const char* path, const std::vector<int>& rk) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) { std::printf("FATAL: cannot open %s for write\n", path); return false; }
  const std::size_t bytes = rk.size() * sizeof(int);
  const bool ok = std::fwrite(rk.data(), 1, bytes, f) == bytes;
  std::fclose(f);
  return ok;
}

// Text sink: N lines x L tokens -- the format Evaluation::evaluate reads back
// (assignFromTextFile) and rfe_quality.py loads without --bin.
bool write_rk_text(const char* path, const std::vector<int>& rk, int N, int L) {
  std::FILE* f = std::fopen(path, "w");
  if (!f) { std::printf("FATAL: cannot open %s for write\n", path); return false; }
  bool ok = true;
  for (int q = 0; q < N && ok; ++q) {
    for (int j = 0; j < L; ++j) {
      if (j && std::fputc(' ', f) == EOF) { ok = false; break; }
      if (std::fprintf(f, "%d", rk[(std::size_t)q * (std::size_t)L + (std::size_t)j]) < 0) {
        ok = false; break;
      }
    }
    if (ok && std::fputc('\n', f) == EOF) ok = false;
  }
  std::fclose(f);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 9) {
    std::printf(
        "uso: bench_rfe_quality <input|legacy|exact|fast> <rk> <N> <L> <k> <t> "
        "<pa> <out.u32> [--bin] [--cc] [--th_cc X] [--threads T]\n");
    return 2;
  }
  const std::string arm = argv[1];
  const char* rkpath = argv[2];
  const int N = std::atoi(argv[3]);
  const int L = std::atoi(argv[4]);
  const int k = std::atoi(argv[5]);
  const int t = std::atoi(argv[6]);
  const double pa = std::atof(argv[7]);
  const char* outpath = argv[8];
  bool bin = false, cc = false;
  double th_cc = 0.0;
  int threads = 0;
  for (int a = 9; a < argc; ++a) {
    if (std::strcmp(argv[a], "--bin") == 0) bin = true;
    else if (std::strcmp(argv[a], "--cc") == 0) cc = true;
    else if (std::strcmp(argv[a], "--th_cc") == 0 && a + 1 < argc) th_cc = std::atof(argv[++a]);
    else if (std::strcmp(argv[a], "--threads") == 0 && a + 1 < argc) threads = std::atoi(argv[++a]);
    else { std::printf("FATAL: unknown arg %s\n", argv[a]); return 2; }
  }
  // Sink format follows the OUT extension: *.txt = text (Evaluation reads it),
  // otherwise raw u32 (rfe_quality.py --bin).
  const std::size_t olen = std::strlen(outpath);
  const bool out_text = olen >= 4 && std::strcmp(outpath + olen - 4, ".txt") == 0;
  if (threads > 0) omp_set_num_threads(threads);

  std::vector<int> rk = bin ? load_rk_bin(rkpath, N, L)
                            : load_rk_text(rkpath, N, L);
  std::printf("RFEQ arm=%s N=%d L=%d k=%d t=%d pa=%g cc=%d threads=%d\n",
              arm.c_str(), N, L, k, t, pa, cc ? 1 : 0, threads);

  const double t0 = now_s();
  if (arm == "input") {
    // arm (a): the input ranking itself, untouched.
  } else if (arm == "legacy") {
    RFERef ref(N, L, k, t, pa);
    ref.load_rk(rk);
    ref.set_th_cc(th_cc);
    ref.run();
    if (cc) ref.exec_rr_ccs();
    rk = ref.rk();
  } else if (arm == "exact") {
    RFECore core(rk, (std::size_t)N, (std::size_t)L, k, t, pa, cc, th_cc);
    core.run();
    rk = core.rk_ref();
  } else if (arm == "fast") {
#if __has_include("Methods/hpp/RFEFastCore.hpp")
    RFEFastCore core(rk, (std::size_t)N, (std::size_t)L, k, t, pa, cc, th_cc);
    core.run();
    rk = core.rk_ref();
#else
    std::printf("FATAL: arm=fast requested but RFEFastCore.hpp does not exist yet "
                "(Fase 2)\n");
    return 3;
#endif
  } else {
    std::printf("FATAL: unknown arm %s\n", arm.c_str());
    return 2;
  }
  const double wall = now_s() - t0;

  if (!(out_text ? write_rk_text(outpath, rk, N, L) : write_rk_bin(outpath, rk)))
    return 2;
  std::printf("RFEQ wall_s=%.3f VmHWM_kb=%ld ck_rk=0x%016llx\n", wall,
              vm_hwm_kb(), (unsigned long long)fnv1a(rk.data(), rk.size() * sizeof(int)));
  std::printf("RFEQ wrote=%s cells=%zu\n", outpath, rk.size());
  return 0;
}

