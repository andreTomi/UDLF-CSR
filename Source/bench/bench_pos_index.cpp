// bench_pos_index.cpp -- Step 1.0 of the F1 plan: CONFIRM before optimising.
//
// F1's profile says t_index (build_position_index) is 47% of F1 at T=12 and it
// is serial. The hypothesis is that 28.8M push_back into 72k std::vector is
// dominated by REALLOCATION, not by writing -- 21.5 ns/element is far too slow
// for a push_back. This benchmark builds the index both ways and reports:
//
//   1. time for each build,
//   2. how many times operator new was called by each (the reallocation count),
//   3. whether the two indices are IDENTICAL, cell for cell.
//
// (3) is the point. Last time I "confirmed" something it was a tautology of my
// own probe. Here identity is checked, not assumed: if the CSR build differed
// anywhere, this says so and the optimisation does not proceed.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <array>

static long long g_new_calls = 0;
void* operator new(std::size_t n) { ++g_new_calls; void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using clk = std::chrono::steady_clock;
static double now_s() {
  return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  const char* path = argv[1];
  const long long N = atoll(argv[2]);
  const int L = atoi(argv[3]);
  std::vector<int> rk((std::size_t)N * (std::size_t)L);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  if (fread(rk.data(), 4, rk.size(), f) != rk.size()) { printf("short read\n"); return 2; }
  fclose(f);
  printf("POSIDX file=%s N=%lld L=%d cells=%lld\n", path, N, L, N * L);

  // ---------------- V1: the current vector<vector> build ----------------
  std::vector<std::vector<std::uint32_t>> inv_rows, inv_pos;
  std::vector<std::size_t> inv_n;
  long long a0 = g_new_calls;
  double t0 = now_s();
  inv_rows.assign((std::size_t)N, {});
  inv_pos.assign((std::size_t)N, {});
  inv_n.assign((std::size_t)N, 0);
  for (std::size_t row = 0; row < (std::size_t)N; ++row) {
    for (int i = 0; i < L; ++i) {
      const int c = rk[(std::size_t)L * row + (std::size_t)i];
      if (c < 0 || c >= (int)N) continue;
      inv_rows[(std::size_t)c].push_back((std::uint32_t)row);
      inv_pos[(std::size_t)c].push_back((std::uint32_t)i);
      ++inv_n[(std::size_t)c];
    }
  }
  double t1 = now_s();
  long long a1 = g_new_calls;
  printf("POSIDX V1 vector<vector> : %.3f s  operator_new calls=%lld  (%.1f ns/element)\n",
         t1 - t0, a1 - a0, 1e9 * (t1 - t0) / (double)(N * L));

  // ---------------- V2: CSR, two passes -----------------------------------
  std::vector<std::uint32_t> rows_flat, pos_flat;
  std::vector<std::size_t> off;
  long long a2 = g_new_calls;
  double t2 = now_s();
  std::vector<std::size_t> cnt((std::size_t)N + 1, 0);
  for (std::size_t row = 0; row < (std::size_t)N; ++row)
    for (int i = 0; i < L; ++i) {
      const int c = rk[(std::size_t)L * row + (std::size_t)i];
      if (c < 0 || c >= (int)N) continue;
      ++cnt[(std::size_t)c + 1];
    }
  off.assign((std::size_t)N + 1, 0);
  for (std::size_t c = 0; c < (std::size_t)N; ++c) off[c + 1] = off[c] + cnt[c + 1];
  rows_flat.assign(off[(std::size_t)N], 0);
  pos_flat.assign(off[(std::size_t)N], 0);
  std::vector<std::size_t> cur(off.begin(), off.end() - 1);
  for (std::size_t row = 0; row < (std::size_t)N; ++row)
    for (int i = 0; i < L; ++i) {
      const int c = rk[(std::size_t)L * row + (std::size_t)i];
      if (c < 0 || c >= (int)N) continue;
      const std::size_t at = cur[(std::size_t)c]++;
      rows_flat[at] = (std::uint32_t)row;
      pos_flat[at] = (std::uint32_t)i;
    }
  double t3 = now_s();
  long long a3 = g_new_calls;
  printf("POSIDX V2 CSR 2-pass    : %.3f s  operator_new calls=%lld  (%.1f ns/element)\n",
         t3 - t2, a3 - a2, 1e9 * (t3 - t2) / (double)(N * L));
  printf("POSIDX speedup = %.2fx\n", (t1 - t0) / (t3 - t2));

  // ---------------- identity, checked not assumed -------------------------
  long long bad_rows = 0, bad_pos = 0, total = 0;
  int shown = 0;
  for (std::size_t c = 0; c < (std::size_t)N; ++c) {
    const std::size_t lo = off[c], hi = off[c + 1];
    if (hi - lo != inv_rows[c].size()) {
      ++bad_rows;
      if (shown < 6) {
        printf("POSIDX DIAG c=%zu csr_size=%zu v1_size=%zu lo=%zu hi=%zu\n",
               c, hi - lo, inv_rows[c].size(), lo, hi);
        ++shown;
      }
      continue;
    }
    total += (long long)(hi - lo);
    for (std::size_t k = lo; k < hi; ++k) {
      if (rows_flat[k] != inv_rows[c][k - lo]) ++bad_rows;
      if (pos_flat[k]  != inv_pos[c][k - lo])  ++bad_pos;
    }
  }
  printf("POSIDX identity: entries=%lld rows_mismatch=%lld pos_mismatch=%lld -> %s\n",
         total, bad_rows, bad_pos,
         (bad_rows == 0 && bad_pos == 0) ? "IDENTICAL" : "DIFFERENT -- DO NOT OPTIMISE");
  return (bad_rows == 0 && bad_pos == 0) ? 0 : 1;
}
