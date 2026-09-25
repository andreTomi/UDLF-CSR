// test_rdpac_corner.cpp
//
// Corner-case suite for the RDPAC sparse port. Two kinds of checks:
//   A) Parity cases: crafted inputs run through BOTH the frozen reference
//      and the core; all S1-S5 phases must match bitwise (run_parity_case).
//   B) Core-only contract checks: inputs the reference cannot express (its
//      P/PL are compile-time constants) or where the legacy code would
//      corrupt memory instead of failing -- the core must throw, and the
//      p=0 division path must produce NaN without UB (sanitizer-clean).
//
// Cases: multi-seed (1..5), 2L==N, tiny N with small k, query moved out of
// position 0 (pin exercised), partitioned ties (stable sort), k_inc=2,
// k_end unreachable by step (store_list never fires), invalid params,
// duplicate id, missing query, out-of-range id, p=0 zero-colsum.
//
// Build (WSL, repo root): see run_all.sh.

#include "rdpac_parity_common.hpp"

#include <cmath>
#include <random>

namespace {

#define CHECK(cond, msg)                                      \
  do {                                                        \
    ++g_checks;                                               \
    if (!(cond)) {                                            \
      std::printf("  [FAIL] %s (line %d)\n", msg, __LINE__);  \
      ++g_failures;                                           \
    } else {                                                  \
      std::printf("  [OK]   %s\n", msg);                      \
    }                                                         \
  } while (0)

std::vector<int> synth(int n, int l, int seed) {
  rdpac_ref::allocate_structures(n, l, 1, 15);
  rdpac_ref::init_synthetic_rkLists(seed);
  return rdpac_ref::rkLists;  // copy
}

// Move every row's self id out of position 0 (to 1 + row%(W-1)). Uniqueness
// is preserved; the S1 sort's query pin must bring it back on both sides.
std::vector<int> move_self(const std::vector<int>& in, int n, int width) {
  std::vector<int> out = in;
  for (int i = 0; i < n; ++i) {
    const int target = 1 + (i % (width - 1));
    std::swap(out[static_cast<size_t>(i) * width + 0],
              out[static_cast<size_t>(i) * width + target]);
  }
  return out;
}

// Two groups of 4 (N=8, L=4, width=8): each row lists self + its 3 group
// peers in the first L, then the 4 foreign ids in the tail. Every tail cell
// scores exactly 0 (foreign rows never list us in their first L) -> a
// 4-way tie per row exercises the stable insertion sort on both sides.
std::vector<int> partition_ties() {
  std::vector<int> rk(8 * 8, -1);
  for (int i = 0; i < 8; ++i) {
    const int g = i / 4;
    std::vector<int> row;
    row.push_back(i);
    for (int j = g * 4; j < g * 4 + 4; ++j)
      if (j != i) row.push_back(j);
    for (int j = (1 - g) * 4; j < (1 - g) * 4 + 4; ++j) row.push_back(j);
    for (int pos = 0; pos < 8; ++pos)
      rk[static_cast<size_t>(i) * 8 + pos] = row[static_cast<size_t>(pos)];
  }
  return rk;
}

// Row i omits its own id entirely (unique, in-range otherwise). The legacy
// kernelSortRankedLists would read one past the row and swap a neighbouring
// row's first element; the core must fail fast instead.
std::vector<int> missing_query() {
  std::vector<int> rk(9 * 8, -1);
  for (int i = 0; i < 9; ++i) {
    int pos = 0;
    for (int j = 0; j < 9; ++j)
      if (j != i) rk[static_cast<size_t>(i) * 8 + pos++] = j;
  }
  return rk;
}

bool run_throws(std::vector<int>& rk, int n, int l, int kw, int ks, int ki,
                int ke) {
  try {
    RDPACSparseCore core(rk, static_cast<size_t>(n), static_cast<size_t>(l),
                         static_cast<size_t>(kw), rdpac_ref::P_D,
                         rdpac_ref::PL, ks, ki, ke);
    core.run();
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  } catch (const std::exception& e) {
    std::printf("  [note] unexpected exception type: %s\n", e.what());
    return false;
  }
}

}  // namespace

int main() {
  std::printf("RDPAC corner cases (reference parity + core contracts)\n");

  // -- A) parity cases ------------------------------------------------------
  for (int seed = 1; seed <= 5; ++seed) {
    const auto in = synth(200, 30, seed);
    const std::string label = "multiseed" + std::to_string(seed);
    run_parity_case(in, 200, 30, 1, 1, 15, label.c_str());
  }

  {  // 2L == N: every row is a full permutation of all ids.
    const auto in = synth(40, 20, 7);
    run_parity_case(in, 40, 20, 1, 1, 15, "2L_eq_N");
  }

  {  // Tiny dataset with k_end below the default 15.
    const auto in = synth(8, 4, 3);
    run_parity_case(in, 8, 4, 1, 1, 4, "tiny_k4");
  }

  {  // Query not at position 0 in the input: pin must fire on both sides.
    const auto base = synth(200, 30, 42);
    const auto in = move_self(base, 200, 60);
    run_parity_case(in, 200, 30, 1, 1, 15, "query_moved");
  }

  {  // Partitioned rows: heavy zero-tail ties (stable-sort parity).
    run_parity_case(partition_ties(), 8, 4, 1, 1, 4, "partition_ties",
                    /*expect_changed=*/false);
  }

  {  // k_inc=2 aligned: store_list fires on the last iteration (15).
    const auto in = synth(200, 30, 42);
    run_parity_case(in, 200, 30, 1, 2, 15, "kinc2");
  }

  {  // k_end unreachable (k=2,5,8,11,14 then 17>15): store_list NEVER
     // fires, nonzero lists stay empty -- both sides must produce all-zero
     // final products and identical rankings.
    const auto in = synth(200, 30, 42);
    run_parity_case(in, 200, 30, 2, 3, 15, "kend_unreachable");
  }

  // -- B) core contract checks ---------------------------------------------
  {
    std::vector<int> rk = synth(8, 4, 1);
    CHECK(run_throws(rk, 8, 4, 8, 1, 1, 9), "k_end > width is rejected");
    CHECK(run_throws(rk, 8, 4, 8, 5, 1, 4), "k_start > k_end is rejected");
    CHECK(run_throws(rk, 8, 4, 8, 1, 1, 0), "k_end <= 0 is rejected");
  }
  {  // width > N with an exactly-sized buffer: the width>N contract fires
     // before the (impossible) uniqueness scan.
    std::vector<int> rk(8 * 10, 0);
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 10; ++j)
        rk[static_cast<size_t>(i) * 10 + j] = (i + j) % 8;
    CHECK(run_throws(rk, 8, 4, 10, 1, 1, 4), "width > N is rejected");
  }
  {  // Duplicate id in a row (buffer size is correct).
    std::vector<int> rk = synth(8, 4, 1);
    rk[1] = rk[0];  // row 0 slot 1 now duplicates slot 0
    CHECK(run_throws(rk, 8, 4, 8, 1, 1, 4), "duplicate id is rejected");
  }
  {  // Out-of-range ids.
    std::vector<int> rk = synth(8, 4, 1);
    rk[2] = -1;
    CHECK(run_throws(rk, 8, 4, 8, 1, 1, 4), "negative id is rejected");
    rk[2] = 99;
    CHECK(run_throws(rk, 8, 4, 8, 1, 1, 4), "id >= N is rejected");
  }
  {  // Self id missing entirely: must fail fast (legacy reads past the row).
    auto rk = missing_query();
    CHECK(run_throws(rk, 9, 4, 8, 1, 1, 4), "missing query is rejected");
  }
  {  // p=0: every column sum is 0 at the first normalization -> 0/0 = NaN
     // (unguarded, exactly as the legacy float code behaves). No UB -- the
     // sanitizer build of this suite proves it -- and the pipeline must
     // still finish with every query pinned at position 0.
    auto rk = synth(60, 30, 11);
    RDPACSparseCore core(rk, 60, 30, 60, /*p=*/0.0, rdpac_ref::PL, 1, 1, 15);
    bool completed = true;
    try {
      core.run();
    } catch (const std::exception& e) {
      completed = false;
      std::printf("  [note] p=0 run threw: %s\n", e.what());
    }
    CHECK(completed, "p=0 (zero colsum -> NaN) completes without UB");
    CHECK(std::isnan(core.rwl(0, 0)), "p=0 first RWL cell is NaN (0/0)");
    bool pinned = true;
    for (int i = 0; i < 60; ++i) pinned = pinned && (core.rk(i, 0) == i);
    CHECK(pinned, "p=0 pipeline still pins every query at position 0");
  }

  return rdpac_result();
}

