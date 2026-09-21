#!/bin/bash
# A/B: rebuild the SAME driver against the SparseMatrix.hpp of each commit.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
cd "$R"
declare -A C=( [base]=86898d5 [f1]=0fba99c [f13]=b3303ec )
for v in base f1 f13; do
  mkdir -p /tmp/inc_$v/Core/hpp
  cp Source/src/Core/hpp/*.hpp /tmp/inc_$v/Core/hpp/
  git show "${C[$v]}:Source/src/Core/hpp/SparseMatrix.hpp" > /tmp/inc_$v/Core/hpp/SparseMatrix.hpp
  g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE \
      -I /tmp/inc_$v -I Source/src \
      Source/bench/bench_phase.cpp -o /tmp/bp_ab_$v -ltbb
  echo "BUILT $v ($(git rev-parse --short ${C[$v]})) -> /tmp/bp_ab_$v"
done
