#!/usr/bin/env bash
# _final_check.sh -- end-of-Etapa-A verification: both C++ suites build and
# their dumps are byte-identical across thread counts (determinism), and the
# characterization suite reports 0 failures.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_csr_ops.cpp \
    -o /tmp/final_ops -ltbb || exit 1
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_multiply_and_prune.cpp \
    -o /tmp/final_map -ltbb || exit 1
echo "builds OK"
for t in 1 2 4 8 12; do
  ops=$(OMP_NUM_THREADS=$t /tmp/final_ops | tail -n +2 | grep -v Build | md5sum | cut -c1-12)
  map_=$(OMP_NUM_THREADS=$t /tmp/final_map | tail -n +2 | grep -v Build | md5sum | cut -c1-12)
  echo "t=$t ops=$ops map=$map_"
done
/tmp/final_ops | tail -1
/tmp/final_map | tail -1
