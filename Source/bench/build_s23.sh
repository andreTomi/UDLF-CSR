#!/bin/bash
# S2.3 passo 2: builder unico v2 -- sem <execution>/TBB, portanto sem -ltbb.
# build_s23.sh -- compila o driver do S2.3 (mesma linha do s22b README:118-119,
# + -DNDEBUG: a evidencia de contrato do S2.3 vem da sonda FRAGPROBE, que nao
# depende de assert). Executar via fix_crlf (repo e CRLF):
#   python3 Source/bench/fix_crlf.py Source/bench/build_s23.sh  # -> /tmp
#   bash /tmp/build_s23.sh
set -e
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -DPHASE_PROFILE -I Source/src \
    Source/bench/bench_phase.cpp -o /home/andre/udlf_bin/bp_s23
echo "BUILD_OK -> /home/andre/udlf_bin/bp_s23"