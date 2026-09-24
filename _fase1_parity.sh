#!/bin/bash
# _fase1_parity.sh -- gates estaticos da Fase 1 (S2.4): suites com -DNDEBUG,
# paridade md5 plain vs -DPHASE_PROFILE (instrumentacao output-neutra) e
# syntax-only do driver nos dois modos. Rodar via fix_crlf (repo e CRLF):
#   python3 Source/bench/fix_crlf.py _fase1_parity.sh && bash /tmp/_fase1_parity.sh
set -e
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR

echo "== NDEBUG variants =="
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -I Source/src Source/tests/test_csr_ops.cpp -o /tmp/tco_nd
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/tm_nd
echo "-- test_csr_ops (NDEBUG) --"
/tmp/tco_nd | tail -1
echo "-- test_multiply_and_prune (NDEBUG) --"
/tmp/tm_nd | grep -E 'ALL TESTS|FAILED'

echo "== parity plain vs -DPHASE_PROFILE (output-neutrality) =="
g++ -std=gnu++2b -O2 -fopenmp -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/tm_pl
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/tests/test_csr_ops.cpp -o /tmp/tco_pf
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src Source/tests/test_multiply_and_prune.cpp -o /tmp/tm_pf

md5_of() { OMP_NUM_THREADS=4 "$1" | tail -n +2 | grep -v Build | md5sum | cut -d' ' -f1; }

A=$(md5_of /tmp/test_csr_ops); B=$(md5_of /tmp/tco_pf)
echo "test_csr_ops           plain=$A flag=$B"
if [ "$A" = "$B" ]; then echo "PARITY ops: PASS"; else echo "PARITY ops: FAIL"; exit 1; fi

A=$(md5_of /tmp/tm_pl); B=$(md5_of /tmp/tm_pf)
echo "test_multiply_and_prune plain=$A flag=$B"
if [ "$A" = "$B" ]; then echo "PARITY mnp: PASS"; else echo "PARITY mnp: FAIL"; exit 1; fi

echo "== driver syntax-only, both modes =="
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -I Source/src -fsyntax-only Source/bench/bench_phase.cpp
echo "SYNTAX plain: OK"
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -DPHASE_PROFILE -I Source/src -fsyntax-only Source/bench/bench_phase.cpp
echo "SYNTAX flag: OK"
echo "PARITY GATES DONE"