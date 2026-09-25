// micro_omp.cpp -- pure OpenMP scaling probe for the local machine.
// Measures at 1..N threads:
//   (1) reduction : one big parallel-for (few barriers) -- best case
//   (2) barrier   : width rounds x 2 omp-for with tiny bodies, mimicking
//                   multiply_l1's per-level phase structure (worst case)
// Build: g++ -O2 -fopenmp micro_omp.cpp -o /tmp/micro_omp
#include <omp.h>

#include <chrono>
#include <cstdio>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
  const size_t n = 2000;
  const size_t width = 128;  // 2L for the n=2000 l=64 bench case
  std::vector<float> a(n, 1.0f), tmp(n, 0.0f), out(n * width, 0.0f);
  const int max_t = omp_get_max_threads();
  std::printf("cpus=%d\n", max_t);
  std::fflush(stdout);

  const int counts[6] = {1, 2, 4, 6, 8, 12};
  for (int ci = 0; ci < 6; ++ci) {
    const int t = counts[ci];
    if (t > max_t) continue;
    omp_set_num_threads(t);

    // (1) reduction: 50M float adds, one implicit barrier at the end.
    double best_red = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
      const size_t big = 50000000;
      std::vector<float> v(big, 1.0f);
      double s = 0.0;
      const auto t0 = Clock::now();
      #pragma omp parallel for reduction(+ : s)
      for (size_t i = 0; i < big; ++i) s += v[i];
      const double ms = ms_since(t0);
      if (ms < best_red) best_red = ms;
      if (static_cast<long long>(s) != static_cast<long long>(big))
        std::printf("BAD SUM\n");
    }

    // (2) barrier storm: width rounds x 2 omp-for (static, n rows each),
    // exactly multiply_l1's shape: 2*width implicit barriers per call.
    double best_bar = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = Clock::now();
      #pragma omp parallel
      {
        for (size_t jc = 0; jc < width; ++jc) {
          #pragma omp for schedule(static)
          for (size_t i = 0; i < n; ++i)
            tmp[i] = a[i] * static_cast<float>(jc + 1);
          #pragma omp for schedule(static)
          for (size_t i = 0; i < n; ++i)
            out[i * width + jc] = tmp[i];
        }
      }
      const double ms = ms_since(t0);
      if (ms < best_bar) best_bar = ms;
    }
    std::printf("threads=%2d reduction_ms=%8.3f  barrier_storm_ms=%8.3f\n", t,
                best_red, best_bar);
    std::fflush(stdout);
  }
  return 0;
}
