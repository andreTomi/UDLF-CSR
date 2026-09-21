#!/usr/bin/env bash
# _md5_ops.sh -- determinism of the characterization suite across thread counts.
set -u
echo "raw (header carries the thread count, so it must differ):"
for t in 1 2 4 8 12; do
  printf 't=%-3s ' "$t"
  OMP_NUM_THREADS=$t /tmp/test_csr_ops 2>/dev/null | grep -v 'Build]' | md5sum
done
echo "header stripped (all five must be identical):"
for t in 1 2 4 8 12; do
  printf 't=%-3s ' "$t"
  OMP_NUM_THREADS=$t /tmp/test_csr_ops 2>/dev/null | tail -n +2 | grep -v 'Build]' | md5sum
done
