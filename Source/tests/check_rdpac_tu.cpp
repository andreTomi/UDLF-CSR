// check_rdpac_tu.cpp -- compile-time instantiation check for the RDPAC
// method TU (scaffold + wired execute()). Build:
//   g++ -std=gnu++23 -fsyntax-only -fopenmp -I./src -I./src/Config/hpp \
//       -I./src/Config/models -I./src/Core/hpp -I./src/Methods/hpp \
//       -I./src/Evaluation/hpp -I./src/Utils/hpp Source/tests/check_rdpac_tu.cpp
#include "Methods/hpp/RDPAC.hpp"

// Force the factory registration symbol to be odr-used.
template class MethodRegister<RDPAC>;

int main() {
  auto method = MethodFactory::createMethod("RDPAC");
  return method ? 0 : 1;
}
