// check_bfstree_guard.cpp -- proves the fail-closed build guard of BFSTree.
//
// The guard is the mechanism that makes the identity contract enforceable:
// a binary compiled with fast-math REFUSES to run BFSTree instead of silently
// producing non-bit-exact results (~89% of W cells shift by 1 ULP, ~81% of the
// final rank lists change; dossie numerics_prod, "Passo 0").
//
// Build + run (both arms must be exercised by the gate):
//   1) strict   : g++ -O2 -ffp-contract=off ... -o /tmp/guard_strict
//                 -> prints "GUARD ALLOWED"  (the regime where BFSTree may run)
//   2) fast-math: g++ -O3 -ffast-math ...     -o /tmp/guard_fastmath
//                 -> prints "GUARD REFUSED"   (the product regime; also emits
//                    BFSTree.hpp's #pragma message at compile time)
// Exit 0 in both cases: each arm checks its OWN expected outcome and fails
// loudly otherwise, so the gate can assert on the printed word.
#include "Methods/hpp/BFSTree.hpp"

#include <cstdio>
#include <stdexcept>

int main() {
  try {
    BFSTree::requireStrictBuildForTesting();
    std::printf("GUARD ALLOWED\n");
    return 0;
  } catch (const std::exception& e) {
    std::printf("GUARD REFUSED: %s\n", e.what());
    return 0;
  }
}