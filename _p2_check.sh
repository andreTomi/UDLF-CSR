#!/bin/bash
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1

echo "=== A. new driver (post-removal), mult w1 1 ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /home/andre/udlf_bin/bp_s23 mult w1 1 > /tmp/s_new_w1.txt 2>&1
echo "exit=$?  lines=$(wc -l < /tmp/s_new_w1.txt)"
tail -3 /tmp/s_new_w1.txt

echo "=== B. old driver (pre-removal, bp_s22), mult w1 1 ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /home/andre/udlf_bin/bp_s22 mult w1 1 > /tmp/s_old_w1.txt 2>&1
echo "exit=$?  lines=$(wc -l < /tmp/s_old_w1.txt)"
tail -3 /tmp/s_old_w1.txt

echo "=== C. new driver, mult w1 3 ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /home/andre/udlf_bin/bp_s23 mult w1 3 > /tmp/s_new_w1b.txt 2>&1
echo "exit=$?  reps=$(grep -ac '^REP,' /tmp/s_new_w1b.txt)"

echo "=== D. new driver, mult w3 2 (small enough to finish) ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 /home/andre/udlf_bin/bp_s23 mult w3 2 > /tmp/s_new_w3.txt 2>&1
echo "exit=$?  reps=$(grep -ac '^REP,' /tmp/s_new_w3.txt)"
grep -a -E "^TSET,|^ABKNOBS,|^REP," /tmp/s_new_w3.txt | head -4

echo "=== E. warnings when the removed knobs are exported ==="
env OMP_NUM_THREADS=2 PHASE_THREADS=2 PHASE_BUILDER=legacy PHASE_FRAGAB=1 /home/andre/udlf_bin/bp_s23 mult w1 2 > /tmp/s_knobs.txt 2>&1
echo "exit=$?"
grep -a -E "warning|^ABKNOBS,|^TSET," /tmp/s_knobs.txt | head -5

echo "=== F. md5 parity: plain vs -DPHASE_PROFILE ==="
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_csr_ops.cpp -o /tmp/ops_plain || exit 1
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/tests/test_csr_ops.cpp -o /tmp/ops_pp || exit 1
echo "ops plain : $(/tmp/ops_plain | tail -n +2 | grep -v Build | md5sum)"
echo "ops ppflag: $(/tmp/ops_pp | tail -n +2 | grep -v Build | md5sum)"
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/map_plain || exit 1
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/map_pp || exit 1
echo "map plain : $(/tmp/map_plain | tail -n +2 | grep -v Build | md5sum)"
echo "map ppflag: $(/tmp/map_pp | tail -n +2 | grep -v Build | md5sum)"
echo "=== CHECK SCRIPT DONE ==="
