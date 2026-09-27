#!/usr/bin/env python3
"""rk_width.py -- token-width census of a rkLists.txt rank-list file.

Why this exists: PARAM_BFSTREE_L=1400 (Source/src/Config/hpp/Validator.hpp) is
the BFSTree production list width, but every real-data harness so far extracted
rkLists with L=400 (Data/phase_profile/etapa_b/a5bis: "cols=400, min_tokens=400").
Whether the ALOI file actually carries >=1400 tokens per row decides the
production configuration (l effective) and therefore the F7 work model
(O(N*L^2)) and the W footprint model (blocks of ~L^2 per query).

Usage:  python3 Source/bench/rk_width.py <rkLists.txt> [max_lines]
Prints: min/max/mean tokens per row, rows with >=400 / >=1400 tokens.
"""
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    cap = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    mn, mx, tot, rows = 1 << 60, 0, 0, 0
    ge400 = ge1400 = 0
    hist = {}
    with open(path, 'r') as f:
        for line in f:
            if not line.strip():
                continue
            n = len(line.split())
            mn = min(mn, n)
            mx = max(mx, n)
            tot += n
            rows += 1
            if n >= 400:
                ge400 += 1
            if n >= 1400:
                ge1400 += 1
            hist[n] = hist.get(n, 0) + 1
            if cap and rows >= cap:
                break
    print("RKWIDTH,file=%s,rows=%d,min=%d,max=%d,mean=%.1f,"
          "rows_ge_400=%d,rows_ge_1400=%d%s"
          % (path, rows, mn, mx, tot / max(rows, 1), ge400, ge1400,
             ",truncated_at=%d" % cap if cap else ""))
    for n in sorted(hist)[:8]:
        print("  width %6d : %8d rows" % (n, hist[n]))
    if len(hist) > 8:
        print("  ... %d distinct widths" % len(hist))
    return 0


if __name__ == '__main__':
    sys.exit(main())
