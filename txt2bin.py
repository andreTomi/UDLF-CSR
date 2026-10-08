#!/usr/bin/env python3
"""Convert text rank lists to binary int32 LE format."""
import sys, struct

src, L, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]

vals = []
with open(src) as f:
    for line in f:
        vals.extend(int(x) for x in line.split())

# Verify: N * L cells
N = len(vals) // L
assert len(vals) == N * L, f"cells={len(vals)} != N*{L}"

with open(out, "wb") as f:
    f.write(struct.pack("<%di" % len(vals), *vals))

print(f"converted {src} -> {out}: N={N} L={L} cells={len(vals)}")