// bench_rdpac.cpp
//
// Serial-vs-parallel benchmark + bit-exactness check for RDPACSparseCore on
// a synthetic ranked-list input (distinct ids per row, same generator as the
// frozen reference harness's init_synthetic_rkLists).
//
// Usage: bench_rdpac [n=2000] [l=64] [k_end=8] [seed=42]
// Prints: threads serial_ms parallel_ms speedup ck_serial ck_parallel match
//         plus the per-phase wall times of the parallel run (hook deltas).
//
// The serial run forces omp_set_num_threads(1); the parallel run restores
// the runtime default. Both must produce the SAME final-rk FNV checksum --
// this is the bench-level bit-exactness proof (the full S1-S5 proof lives in
// test_rdpac_parity.cpp against the dense reference).
//
// Wait policy (measured on i5-10400F, 6 cores / 12 SMT threads, WSL):
// libgomp's default ACTIVE spin-wait inside multiply_l1's per-level
// barriers lets the 12 waiting threads starve the 6 hardware threads they
// are waiting on, so the parallel run goes SLOWER than serial on the small
// case (n=2000: 0.45x). OMP_WAIT_POLICY=PASSIVE fixes it with no effect on
// results (match=YES under both policies):
//   n=2000  k=8 : default 0.45x -> PASSIVE 1.85-2.03x
//   n=10000 k=15: default 2.6x  -> PASSIVE 4.0-4.9x
// The Makefile/run_all.sh invocations set PASSIVE accordingly; the printed
// header reports the policy in force so numbers stay attributable.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
//       -I Source/src/Core/hpp Source/bench/bench_rdpac.cpp -o /tmp/bench_rdpac

#include "Methods/hpp/RDPACSparseCore.hpp"

#include <omp.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using clock_t_ = std::chrono::steady_clock;

uint64_t fnv_rk(const std::vector<int>& rk, size_t n, size_t width) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < width; ++j) {
      h ^= static_cast<uint64_t>(static_cast<uint32_t>(rk[i * width + j]));
      h *= 1099511628211ULL;
    }
  }
  return h;
}

using PhaseTimes = std::vector<std::pair<std::string, double>>;

double run_once(const std::vector<int>& base, size_t n, size_t l, size_t width,
                int k_end, uint64_t& ck, PhaseTimes* phases) {
  std::vector<int> rk = base;
  RDPACSparseCore core(rk, n, l, width, 0.60, 0.99, 1, 1, k_end);
  auto t_prev = clock_t_::now();
  const auto t0 = t_prev;
  core.run([&](const char* ph) {
    const auto t = clock_t_::now();
    if (phases) {
      phases->emplace_back(
          ph, std::chrono::duration<double, std::milli>(t - t_prev).count());
    }
    t_prev = t;
  });
  const auto t1 = clock_t_::now();
  ck = fnv_rk(rk, n, width);
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2000;
  const size_t l = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 64;
  const int k_end = (argc > 3) ? std::atoi(argv[3]) : 8;
  const int seed = (argc > 4) ? std::atoi(argv[4]) : 42;
  const size_t width = 2 * l;
  if (n < width) {
    std::fprintf(stderr, "bench_rdpac: need n >= 2l (got n=%zu l=%zu)\n", n, l);
    return 2;
  }

  const int max_threads = omp_get_max_threads();

  // Synthetic input: row i = [i] + (width-1) distinct random ids.
  std::mt19937 rng(seed);
  std::vector<int> base(n * width, 0);
  std::vector<int> pool;
  for (size_t i = 0; i < n; ++i) {
    pool.clear();
    pool.reserve(n - 1);
    for (size_t j = 0; j < n; ++j)
      if (j != i) pool.push_back(static_cast<int>(j));
    std::shuffle(pool.begin(), pool.end(), rng);
    base[i * width] = static_cast<int>(i);
    for (size_t pos = 1; pos < width; ++pos)
      base[i * width + pos] = pool[pos - 1];
  }

  uint64_t ck_serial = 0, ck_parallel = 0;
  PhaseTimes phases_serial, phases_parallel;
  omp_set_num_threads(1);
  const double ms_serial =
      run_once(base, n, l, width, k_end, ck_serial, &phases_serial);
  omp_set_num_threads(max_threads);
  const double ms_parallel =
      run_once(base, n, l, width, k_end, ck_parallel, &phases_parallel);

  const double speedup = (ms_parallel > 0.0) ? ms_serial / ms_parallel : 0.0;
  const char* policy = std::getenv("OMP_WAIT_POLICY");
  std::printf(
      "RDPAC bench: n=%zu l=%zu width=%zu k=1..%d seed=%d\n"
      "threads=%d wait_policy=%s serial_ms=%.2f parallel_ms=%.2f "
      "speedup=%.2fx\n"
      "ck_serial=0x%016llx ck_parallel=0x%016llx match=%s\n",
      n, l, width, k_end, seed, max_threads,
      (policy && *policy) ? policy : "ACTIVE(default)", ms_serial, ms_parallel,
      speedup, static_cast<unsigned long long>(ck_serial),
      static_cast<unsigned long long>(ck_parallel),
      (ck_serial == ck_parallel) ? "YES" : "NO");
  std::printf("%-14s %12s %12s\n", "PHASE", "serial_ms", "par_ms");
  for (size_t i = 0; i < phases_serial.size(); ++i) {
    const double par_ms =
        (i < phases_parallel.size()) ? phases_parallel[i].second : 0.0;
    std::printf("%-14s %12.2f %12.2f\n", phases_serial[i].first.c_str(),
                phases_serial[i].second, par_ms);
  }
  return (ck_serial == ck_parallel) ? 0 : 1;
}
