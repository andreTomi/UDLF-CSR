#!/usr/bin/env python3
"""Same probe, for the raw int32 .bin dumps (ALOI). Format: N x L int32 LE."""
import sys, array
path = sys.argv[1]
N = int(sys.argv[2]); L = int(sys.argv[3])
a = array.array("i")
with open(path, "rb") as f:
    a.fromfile(f, N * L)
print("PROBE file=%s N=%d L=%d" % (path, N, L))
inv = {}
for q in range(N):
    base = q * L
    for i in range(L):
        c = a[base + i]
        d = inv.get(c)
        if d is None:
            inv[c] = {q: i + 1}
        else:
            d[q] = i + 1
eq = diff = absent = 0
distinct = set()
for q in range(N):
    base = q * L
    for i in range(L):
        img = a[base + i]
        direct = i + 1
        d = inv.get(q)
        recip = d.get(img) if d is not None else None
        if recip is None:
            absent += 1
            recip = L + 2
        distinct.add(recip)
        if recip == direct:
            eq += 1
        else:
            diff += 1
total = N * L
print("PROBE cells=%d direct==reciprocal=%d (%.4f%%)  direct!=reciprocal=%d" %
      (total, eq, 100.0 * eq / total, diff))
print("PROBE reciprocal==l+2 (absent) = %d (%.4f%%)" % (absent, 100.0 * absent / total))
print("PROBE distinct reciprocal values = %d" % len(distinct))
print("PROBE reciprocal range = [%d, %d]" % (min(distinct), max(distinct)))
