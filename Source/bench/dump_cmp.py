#!/usr/bin/env python3
"""Compare two canonical builder dumps (S2.1).

The driver `checksum` is a weak invariant (a double accumulation ordered by
column), so the S2 equivalence gate uses the canonical dump instead. This tool
splits a dump difference into the two things that matter:

  * STRUCTURAL: header (rows/cols/nnz) or the column sequence differ -> the
    builder is wrong, stop.
  * VALUE-ORDER: identical rows/cols, only some float bits differ -> a sum of
    >= 3 duplicate records was accumulated in a different order, which BOTH
    builders leave unspecified (std::sort is unstable; par_unseq is not
    deterministic either). Reported as a count, max |delta| and the affected
    cells, so the claim is measurable and bounded instead of hand-waved.

Layout: rows, cols, nnz (u64) | row_ptr rows+1 (u64) | per row: col u32, val u32
matches bench_phase.cpp dump_matrix().

Usage: python3 Source/bench/dump_cmp.py <a.bin> <b.bin> [max_report=10]
"""
import bisect
import struct
import sys


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def compare(pa, pb, max_report=10):
    """Bounded-memory comparison: byte-level fast path per chunk, per-entry
    scan only for the chunks that actually differ."""
    fa = open(pa, 'rb')
    fb = open(pb, 'rb')
    ha = struct.unpack('<3Q', fa.read(24))
    hb = struct.unpack('<3Q', fb.read(24))
    print(f'   header a: rows={ha[0]} cols={ha[1]} nnz={ha[2]}')
    print(f'   header b: rows={hb[0]} cols={hb[1]} nnz={hb[2]}')
    if ha != hb:
        print('   STRUCTURAL: header mismatch -> STOP')
        return 2
    rows, nnz = ha[0], ha[2]
    rpa = struct.unpack('<%dQ' % (rows + 1), fa.read(8 * (rows + 1)))
    rpb = struct.unpack('<%dQ' % (rows + 1), fb.read(8 * (rows + 1)))
    if rpa != rpb:
        bad = [i for i in range(len(rpa)) if rpa[i] != rpb[i]]
        print(f'   STRUCTURAL: row_ptr differs at {len(bad)} rows '
              f'(first={bad[:5]}) -> STOP')
        return 2
    print('   structural: row_ptr identical')

    CH = 1 << 20                      # entries per chunk (8 MB of bytes)
    pos = n_diff = col_bad = 0
    worst = 0.0
    worst_rel = 0.0
    samples = []
    while True:
        ba = fa.read(CH * 8)
        bb = fb.read(CH * 8)
        if not ba and not bb:
            break
        if ba == bb:                  # C-level fast path: chunk identical
            pos += len(ba) // 8
            continue
        va = struct.unpack('<%dI' % (len(ba) // 4), ba)
        vb = struct.unpack('<%dI' % (len(bb) // 4), bb)
        ca, bia = va[0::2], va[1::2]
        cb, bib = vb[0::2], vb[1::2]
        for i in range(len(ca)):
            if ca[i] != cb[i]:
                col_bad += 1
                continue
            if bia[i] != bib[i]:
                n_diff += 1
                x, y = f32(bia[i]), f32(bib[i])
                d = abs(x - y)
                if d > worst:
                    worst = d
                rel = d / max(1e-30, max(abs(x), abs(y)))
                if rel > worst_rel:
                    worst_rel = rel
                if len(samples) < max_report:
                    r = bisect.bisect_right(rpa, pos + i) - 1
                    samples.append((r, ca[i], x, y, d))
        pos += len(ca)
    fa.close()
    fb.close()
    if col_bad:
        print(f'   STRUCTURAL: column sequence differs at {col_bad} entries '
              f'-> STOP')
        return 2
    if n_diff == 0:
        print(f'   values: BIT-IDENTICAL ({nnz} entries)')
        return 0
    print(f'   structural: columns identical | values: {n_diff} of {nnz} '
          f'entries differ ({100.0 * n_diff / max(1, nnz):.4f}%)')
    print(f'   max |delta| = {worst:.6f}  ({worst_rel:.2e} relative to the '
          f'affected values)')
    print('   samples (row, col, a, b, delta):')
    for r, c, x, y, d in samples:
        print(f'     ({r}, {c}) {x!r} vs {y!r} delta={d:.6f}')
    if n_diff > max_report:
        print(f'     ... {n_diff - max_report} more')
    return 1


def main():
    tag = sys.argv[1].split('/')[-1]
    print(f'== {tag} ==')
    mr = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    rc = compare(sys.argv[1], sys.argv[2], mr)
    print('   RESULT:', {0: 'EQUIVALENT (bit-identical)',
                         1: 'columns equal, values differ in the last bits',
                         2: 'STRUCTURAL DIFFERENCE'}[rc])
    return rc


if __name__ == '__main__':
    sys.exit(main())


