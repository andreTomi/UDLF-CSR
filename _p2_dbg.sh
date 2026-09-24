#!/bin/bash
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
echo "=== build -O0 -g, PHASE_PROFILE, asserts ON ==="
g++ -std=gnu++2b -O0 -g -fopenmp -DPHASE_PROFILE -I Source/src Source/bench/bench_phase.cpp -o /tmp/bp_dbg
echo "build exit=$?"
ls -la /tmp/bp_dbg || exit 1

echo "=== run: T=2 mult w1 2 ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /tmp/bp_dbg mult w1 2 > /tmp/dbg.txt 2>&1
echo "run exit=$?"
tail -12 /tmp/dbg.txt

echo "=== run: T=1 mult w1 2 ==="
env OMP_NUM_THREADS=1 PHASE_THREADS=1 /tmp/bp_dbg mult w1 2 > /tmp/dbg1.txt 2>&1
echo "run1 exit=$?"
tail -6 /tmp/dbg1.txt

echo "=== run: T=1 mult w1 2, NDEBUG (=release binary semantics) ==="
g++ -std=gnu++2b -O2 -g -DNDEBUG -fopenmp -DPHASE_PROFILE -I Source/src Source/bench/bench_phase.cpp -o /tmp/bp_dbg_nd
echo "build2 exit=$?"
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /tmp/bp_dbg_nd mult w1 2 > /tmp/dbg2.txt 2>&1
echo "run2 exit=$?"
tail -6 /tmp/dbg2.txt
echo "DONE"
