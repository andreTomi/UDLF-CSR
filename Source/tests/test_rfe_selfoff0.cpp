// test_rfe_selfoff0.cpp -- H19 gate, resolved as option (a): the input
// contract requires the row to CONTAIN its own id at ANY position; self at
// index 0 is pinned later, by the F1 kernel sort, exactly as the legacy does.
//
// Three surfaces, all required before Etapa 6 (which depends on the ALOI file:
// 552/72000 rows with self off index 0, 0 rows without self -- FINDINGS H19):
//   1. Refusals kept HARD: self absent, id out of [0,n), duplicate id.
//   2. Parity on SELF-DISPLACED fixtures (early/late/mixed + synthetic
//      cyclic): F1 (ck_rk + every matrix cell) and the FULL run() final rk,
//      core vs the verbatim reference, bit-exactly.
//   3. RKSELF_OFF0 counter == the fixture's displaced-row count, so the
//      informational log can never silently diverge from reality.
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src
//       -I Source/src/Core/hpp -I Source/tests
//       Source/tests/test_rfe_selfoff0.cpp -o /tmp/test_rfe_selfoff0
// Run from the repo root.

#include "Methods/hpp/RFECore.hpp"
#include "rfe_ref_verbatim.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using CT = CSRMatrix<float>::size_type;

int g_fail = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what);
    ++g_fail;
  } else {
    std::printf("  ok  : %s\n", what);
  }
}

std::vector<int> load_bin(const std::string& path, std::size_t expect_n) {
  std::vector<int> rk(expect_n, 0);
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::printf("FAIL: cannot open %s\n", path.c_str());
    ++g_fail;
    return rk;
  }
  f.read(reinterpret_cast<char*>(rk.data()), (std::streamsize)(expect_n * sizeof(int)));
  if ((std::size_t)f.gcount() != expect_n * sizeof(int)) {
    std::printf("FAIL: short read %s\n", path.c_str());
    ++g_fail;
  }
  return rk;
}

// Cyclic base rows (self at 0), then swap(0, disp) so self sits at `disp`.
std::vector<int> cyclic_displaced(int n, int l, int disp) {
  std::vector<int> rk((std::size_t)n * (std::size_t)l, 0);
  for (int q = 0; q < n; ++q) {
    for (int i = 0; i < l; ++i) rk[(std::size_t)q * l + i] = (q + i) % n;
    std::swap(rk[(std::size_t)q * l], rk[(std::size_t)q * l + disp]);
  }
  return rk;
}

long long count_off0(const std::vector<int>& rk, int n, int l) {
  long long c = 0;
  for (int q = 0; q < n; ++q)
    if (rk[(std::size_t)q * l] != q) ++c;
  return c;
}

// ---- surface 1: the hard refusals -----------------------------------------
void refusal(const char* name, std::vector<int> rk, int n, int l, int k,
             const char* substring) {
  try {
    RFECore core(rk, (std::size_t)n, (std::size_t)l, k, 2, 0.1);
    core.run();
    std::printf("  FAIL: %s: run() did NOT refuse\n", name);
    ++g_fail;
  } catch (const std::runtime_error& e) {
    const bool hit = std::string(e.what()).find(substring) != std::string::npos;
    if (!hit) std::printf("    got: %s\n", e.what());
    expect(hit, (std::string("refuses ") + name).c_str());
  }
}

// ---- surface 2: parity on displaced input ---------------------------------
void parity_case(const char* name, const std::vector<int>& rk_in, int n, int l,
                 int k, int t, double pa, long long expected_off0) {
  std::printf("CASE %s n=%d l=%d k=%d off0_expected=%lld\n", name, n, l, k,
              expected_off0);

  // (a) F1 only: fresh instances, rank_norm (legacy) vs f1_rank_norm (core).
  {
    std::vector<int> a = rk_in, b = rk_in;
    RFERef ref(n, l, k, t, pa);
    ref.load_rk(a);
    ref.rank_norm();
    RFECore core(b, (std::size_t)n, (std::size_t)l, k, t, pa);
    core.f1_rank_norm();
    std::printf("    F1 ck_rk legacy=0x%016llx core=0x%016llx\n",
                (unsigned long long)ref.ck_rk(),
                (unsigned long long)core.ck_rk());
    expect(ref.ck_rk() == core.ck_rk(),
           "F1: ck_rk identical on self-displaced input");
    long bad = 0;
    for (int q = 0; q < n; ++q)
      for (int j = 0; j < n; ++j) {  // full n*n: written cells AND zero-fill
        const float lv = ref.matrix()[(std::size_t)q * n + j];
        const float cv = core.sim().get((CT)q, (CT)j);
        if (lv != cv) {
          if (bad == 0)
            std::printf("    first F1 matrix diff at [%d,%d]: legacy=%.9g core=%.9g\n",
                        q, j, lv, cv);
          ++bad;
          break;
        }
      }
    expect(bad == 0, "F1: every matrix cell matches the legacy (n*n diff)");
  }

  // (b) full pipeline through the production entry point (X9).
  {
    std::vector<int> a = rk_in, b = rk_in;
    RFERef ref(n, l, k, t, pa);
    ref.load_rk(a);
    ref.run();
    RFECore core(b, (std::size_t)n, (std::size_t)l, k, t, pa);
    core.run();
    expect(core.self_off0() == expected_off0,
           "RKSELF_OFF0 counter == the fixture's displaced rows");
    std::printf("    final ck_rk legacy=0x%016llx core=0x%016llx\n",
                (unsigned long long)ref.ck_rk(),
                (unsigned long long)core.ck_rk());
    expect(ref.ck_rk() == core.ck_rk(),
           "run(): final rk digest identical (self pinned by the F1 sort)");
    long bad = 0;
    for (int q = 0; q < n; ++q)
      for (int i = 0; i < l; ++i)
        if (ref.rk()[(std::size_t)q * l + i] !=
            core.rk_ref()[(std::size_t)q * l + i]) {
          ++bad;
          break;
        }
    expect(bad == 0, "run(): every final rank cell matches the legacy");
  }
}

}  // namespace

int main() {
  const double pa = 0.1;

  // ---- surface 1: the three refusals that must stay HARD (H19 option (a)
  //      relaxes ONLY "self at index 0"; everything else keeps refusing) ----
  const int N0 = 32, L0 = 8;
  {
    std::vector<int> base = cyclic_displaced(N0, L0, 0);  // self at 0 (control)
    // self absent: drop id 3 from row 3, shift the tail in (all in range,
    // distinct).
    std::vector<int> no_self = base;
    for (int i = 0; i < L0 - 1; ++i)
      no_self[(std::size_t)3 * L0 + i] = base[(std::size_t)3 * L0 + i + 1];
    no_self[(std::size_t)3 * L0 + L0 - 1] = 3 + L0;  // 11, not present, != 3
    refusal("self absent from the row", no_self, N0, L0, 4,
            "does not contain its own id");

    std::vector<int> oor = base;
    oor[(std::size_t)5 * L0 + 2] = 9999;
    refusal("id out of [0,n)", oor, N0, L0, 4, "out of range");

    std::vector<int> dup = base;
    dup[(std::size_t)7 * L0 + 5] = dup[(std::size_t)7 * L0 + 4];
    refusal("duplicate id in a row", dup, N0, L0, 4, "duplicate id");
  }

  // ---- surface 2: parity on self-displaced input (both directions) --------
  const int t = 2;
  {
    const char* names[] = {"cc_selfoff_early", "cc_selfoff_late",
                           "cc_selfoff_mixed"};
    for (const char* name : names) {
      const std::string path =
          std::string("Data/fixtures/cc/") + name + ".bin";
      std::vector<int> rk = load_bin(path, (std::size_t)500 * 100);
      if (g_fail && rk.empty()) return 1;
      parity_case(name, rk, 500, 100, 10, t, pa, 500);
    }
  }
  // Synthetic: self at position 3 of every row of a cyclic fixture.
  parity_case("cyclic_disp3", cyclic_displaced(N0, L0, 3), N0, L0, 4, t, pa,
              N0);

  if (g_fail) {
    std::printf("RFE SELF-OFF0 (H19): %d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("RFE SELF-OFF0 (H19): OK (refusals hard, displaced parity "
              "bit-identical, RKSELF_OFF0 truthful)\n");
  return 0;
}