// check_instantiations.cpp
//
// Compile-time coverage check: forces instantiation of every public static
// member of CSRMatrix used by the production callers (CprrSparse / LHRRSparse):
// multiply_and_prune, merge, combine, transpose -- for float and double.
//
// Build: g++ -std=gnu++2b -fsyntax-only -fopenmp -I Source/src $this

#include "Core/hpp/SparseMatrix.hpp"

using MF = CSRMatrix<float>;
using MD = CSRMatrix<double>;

using AddF = float (*)(float, float);
using AddD = double (*)(double, double);

static float add_f(float a, float b) { return a + b; }
static double add_d(double a, double b) { return a + b; }

// Taking the address of a static template member forces its instantiation.
const auto f_map    = &MF::multiply_and_prune;
const auto f_merge  = &MF::merge<AddF>;
const auto f_comb   = &MF::combine;
const auto f_tr     = &MF::transpose;
const auto d_map    = &MD::multiply_and_prune;
const auto d_merge  = &MD::merge<AddD>;
const auto d_comb   = &MD::combine;
const auto d_tr     = &MD::transpose;

int main() {
    return (f_map && f_merge && f_comb && f_tr &&
            d_map && d_merge && d_comb && d_tr) ? 0 : 1;
}
