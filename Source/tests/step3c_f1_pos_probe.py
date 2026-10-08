#!/usr/bin/env python3
"""step3c_f1_pos_probe.py -- Step 0 of the F1 plan.

Question: in the legacy's rankNorm, is the DIRECT rank (i+1) equal to the
RECIPROCAL rank (getPosition(img_rl_i, q), "where does q sit in img's list")?

The core computes the reciprocal via position_of() and passes it TWICE into
rank_norm_cell, discarding the direct rank. The legacy does the same
(rfe_ref_verbatim.hpp:320-323). If they are equal everywhere, the core is right
AND the duplicate-parameter form at RFECore.hpp:974 is load-bearing -- which is
a fragility the next person must not "clean up".

If they are NOT equal, the core has a real bug and everything downstream of this
plan is wrong. So this runs first.

Also reports distinct reciprocal values: that is the Step 1 decision input for
the lookup-table hypothesis (x lives in a tiny set vs millions).
"""
import sys

path = sys.argv[1]
rows = []
with open(path) as f:
    for line in f:
        t = line.split()
        if t:
            rows.append([int(x) for x in t])
N = len(rows)
L = len(rows[0])
print("PROBE file=%s N=%d L=%d" % (path, N, L))

# inv[c] = {row q : position of c in row q, 1-based}
inv = {}
for q, r in enumerate(rows):
    for i, c in enumerate(r):
        d = inv.get(c)
        if d is None:
            inv[c] = {q: i + 1}
        else:
            d[q] = i + 1

eq = diff = absent = 0
distinct = set()
examples = []
for q, r in enumerate(rows):
    for i, img in enumerate(r):
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
            if len(examples) < 5:
                examples.append((q, i, img, direct, recip))

total = N * L
print("PROBE cells=%d direct==reciprocal=%d (%.4f%%)  direct!=reciprocal=%d" %
      (total, eq, 100.0 * eq / total, diff))
print("PROBE reciprocal==l+2 (absent marker) = %d (%.4f%%)" % (absent, 100.0 * absent / total))
print("PROBE distinct reciprocal values = %d   (table hypothesis needs this to be SMALL)" % len(distinct))
if distinct:
    print("PROBE reciprocal range = [%d, %d]" % (min(distinct), max(distinct)))
for e in examples:
    print("PROBE example q=%d i=%d img=%d direct=%d reciprocal=%d" % e)
