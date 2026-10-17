// Product translation-unit check for CPRR_SPARSE factory registration.
#include "Methods/hpp/CprrSparse.hpp"

template class MethodRegister<CprrSparse>;

int main() {
  auto method = MethodFactory::createMethod("CPRR_SPARSE");
  return method ? 0 : 1;
}