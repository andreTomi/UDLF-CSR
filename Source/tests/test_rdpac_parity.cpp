// test_rdpac_parity.cpp
//
// Phase-by-phase bit-exact parity gate: RDPACSparseCore vs the frozen
// verbatim reference harness, N=200/L=30/k=1..15 (the ALOI config shape).
// All machinery lives in rdpac_parity_common.hpp (shared with the
// corner-case suite).
//
// Build (WSL, repo root):
//   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp \
//       -I Source/src -I Source/src/Core/hpp -I Source/tests \
//       Source/tests/test_rdpac_parity.cpp -o /tmp/rdpac_parity_gate
// Run: /tmp/rdpac_parity_gate   (exit 0 = every phase bit-identical)
//
// -ffp-contract=off proves ALGORITHM parity under strict IEEE on both sides
// (both kernels live in this TU, same flags); the release build's
// -ffast-math is validated separately by the framework suite.

#include "rdpac_parity_common.hpp"

int main() {
  std::printf(
      "RDPAC parity gate: core vs verbatim reference "
      "(P=%.2f PL=%.2f, seed 42)\n",
      rdpac_ref::P_D, rdpac_ref::PL);

  rdpac_ref::allocate_structures();
  rdpac_ref::init_synthetic_rkLists(42);
  const std::vector<int> input = rdpac_ref::rkLists;

  run_parity_case(input, rdpac_ref::N, rdpac_ref::L, rdpac_ref::K_START,
                  rdpac_ref::K_INC, rdpac_ref::K_END, "seed42");
  return rdpac_result();
}
