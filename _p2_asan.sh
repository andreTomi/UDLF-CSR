#!/bin/bash
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
echo "=== build with ASan ==="
g++ -std=gnu++2b -O1 -g -fopenmp -fsanitize=address -DPHASE_PROFILE -I Source/src Source/bench/bench_phase.cpp -o /tmp/bp_asan
echo "build exit=$?"
echo "=== run T=1 mult w1 1 under ASan ==="
env OMP_NUM_THREADS=1 PHASE_THREADS=1 ASAN_OPTIONS=detect_leaks=0 /tmp/bp_asan mult w1 1 > /tmp/asan.txt 2>&1
echo "run exit=$?"
echo "=== sanitizer report ==="
grep -a -n -E "ERROR: AddressSanitizer|SUMMARY|READ of size|WRITE of size|SEGV|is located|allocated by thread" /tmp/asan.txt | head -25
echo "=== first 60 lines of the report ==="
grep -a -n -A 22 "AddressSanitizer" /tmp/asan.txt | head -45
echo "DONE"
