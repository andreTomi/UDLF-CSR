// check_bfstree_tu.cpp -- compile-time instantiation check for the BFSTree
// method TU (A4 carrier + factory registration). Build (WSL, repo root):
//   g++ -std=gnu++23 -fsyntax-only -fopenmp -I./src -I./src/Config/hpp \
//       -I./src/Config/models -I./src/Core/hpp -I./src/Methods/hpp \
//       -I./src/Evaluation/hpp -I./src/Utils/hpp Source/tests/check_bfstree_tu.cpp
// NOTE: this file intentionally includes the header WITHOUT fast-math: the
// method refuses to run when built with it (see Methods/hpp/BFSTree.hpp).
#include "Methods/hpp/BFSTree.hpp"

// Force the factory registration symbol to be odr-used.
template class MethodRegister<BFSTree>;

int main() {
  auto method = MethodFactory::createMethod("BFSTree");
  return method ? 0 : 1;
}