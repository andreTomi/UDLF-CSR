// check_rfe_guard.cpp -- RFE strict-build guard test.
// Exercised by test-rfe and run_all.sh:
//   strict build (-O2 -ffp-contract=off) -> GUARD ALLOWED
//   fast-math build (-ffast-math)       -> GUARD REFUSED
#include "Methods/hpp/RFE.hpp"
#include <iostream>

int main() {
  try {
    RFE::requireStrictBuildForTesting();
    std::cout << "GUARD ALLOWED\n";
    return 0;
  } catch (const std::runtime_error& e) {
    std::cout << "GUARD REFUSED: " << e.what() << "\n";
    return 1;
  }
}