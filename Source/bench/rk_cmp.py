#!/usr/bin/env python3
"""Layer-0 ranked-list comparison for the S2 builder A/B (S2.2a tooling).

Usage: python3 Source/bench/rk_cmp.py <a.bin> <b.bin> <N> <L> [max_detail=200]

Inputs are the raw row-major u32 N*L binaries written by the driver knob
PHASE_RK_OUT (the reranked lists run_real ends with). Splits a difference
into the S2.2b quantities:

  IDENTICAL      every row bit-equal -> the builders agree at layer 0.
  DIFF_ROWS      rows whose L ids differ (WIDE divergence => STOP and report,
                 per the S2.2b mandate).
  position swaps positions j where a[i][j] != b[i][j] (all differing rows).
  set divergence |set(a_row) symdiff set(b_row)| (all differing rows).
  Kendall tau    over the elements common to both rows of a row (exact when
                 the row sets are equal; ties excluded). Computed for the
                 first `max_detail` differing rows only (O(L log L) each).

Exit: 0 identical, 1 divergence, 2 usage/format error.
Layout check: the file size must be exactly N*L*4 (structural gate).
"""
import struct
import sys
from bisect import bisect_right

MAX_STAT_ROWS = 200000  # beyond this, per-row stats are computed on a sample


def die(msg):
    print('ERROR: ' + msg)
    sys.exit(2)


def load(path, nbytes):
    with open(path, 'rb') as f:
        buf = f.read()
    if len(buf) != nbytes:
        die('%s: %d bytes, expected %d (N*L*4) -> format mismatch'
            % (path, len(buf), nbytes))
    return buf


def inversions(seq):
    """Pairwise inversions via insertion into a sorted prefix: O(L log L)."""
    seen = []
    inv = 0
    for x in seq:
        p = bisect_right(seen, x)
        inv += len(seen) - p
        seen.insert(p, x)
    return inv


def kendall(ra, rb):
    """Kendall tau for two rows restricted to their common elements.

    Returns (tau, m); tau is None when fewer than 2 elements are common or
    the restrictions are not permutations of the common set (duplicate ids).
    """
    common = set(ra) & set(rb)
    m = len(common)
    if m < 2:
        return None, m

    def restrict(row):
        seen = set()
        out = []
        for x in row:
            if x in common and x not in seen:
                seen.add(x)
                out.append(x)
        return out

    ua, ub = restrict(ra), restrict(rb)
    if len(ua) != m or len(ub) != m:
        return None, m
    rank = {v: i for i, v in enumerate(ua)}
    tau = 1.0 - 2.0 * inversions([rank[v] for v in ub]) / (m * (m - 1))
    return tau, m


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    pa, pb = sys.argv[1], sys.argv[2]
    N, L = int(sys.argv[3]), int(sys.argv[4])
    max_detail = int(sys.argv[5]) if len(sys.argv) > 5 else 200
    if N <= 0 or L <= 0:
        die('N and L must be positive')
    nbytes = N * L * 4
    print('== %s vs %s ==' % (pa, pb))
    A = load(pa, nbytes)
    B = load(pb, nbytes)
    if A == B:
        print('   rows=%d cols=%d: BIT-IDENTICAL (%d rows)' % (N, L, N))
        print('   RESULT: IDENTICAL')
        return 0

    rl = L * 4
    diff = [i for i in range(N)
            if A[i * rl:(i + 1) * rl] != B[i * rl:(i + 1) * rl]]
    print('   rows=%d cols=%d: IDENTICAL_ROWS=%d DIFF_ROWS=%d (%.4f%%)'
          % (N, L, N - len(diff), len(diff), 100.0 * len(diff) / N))
    if not diff:
        print('   RESULT: IDENTICAL')
        return 0
    if len(diff) > N // 100:
        print('   WIDE DIVERGENCE (>1% of rows): per the S2.2b mandate, '
              'STOP and report before any timing.')

    fmt = '<%dI' % L
    stat_rows = diff[:MAX_STAT_ROWS]
    tau_rows = diff[:max_detail]
    tau_set = set(tau_rows)
    mm_min = mm_max = mm_sum = 0
    sd_min = sd_max = sd_sum = 0
    same_set = 0
    n_stat = 0
    taus = []
    samples = []
    for i in stat_rows:
        off = i * rl
        ra = struct.unpack_from(fmt, A, off)
        rb = struct.unpack_from(fmt, B, off)
        mm = 0
        for p, q in zip(ra, rb):
            if p != q:
                mm += 1
        sd = len(set(ra) ^ set(rb))
        if n_stat == 0:
            mm_min = mm_max = mm
            sd_min = sd_max = sd
        else:
            mm_min = min(mm_min, mm)
            mm_max = max(mm_max, mm)
            sd_min = min(sd_min, sd)
            sd_max = max(sd_max, sd)
        mm_sum += mm
        sd_sum += sd
        n_stat += 1
        if sd == 0:
            same_set += 1
        if i in tau_set:
            tau, _m = kendall(ra, rb)
            if tau is not None:
                taus.append(tau)
            if len(samples) < 10:
                samples.append((i, mm, sd, tau))

    print('   position swaps/row: min=%d mean=%.2f max=%d (over %d rows)'
          % (mm_min, mm_sum / n_stat, mm_max, n_stat))
    print('   set divergence/row: min=%d mean=%.2f max=%d | same-set rows=%d'
          % (sd_min, sd_sum / n_stat, sd_max, same_set))
    if n_stat < len(diff):
        print('   ... stats cover the first %d of %d differing rows'
              % (n_stat, len(diff)))
    if taus:
        print('   Kendall tau (common elements, %d of %d sampled rows): '
              'min=%.6f mean=%.6f'
              % (len(taus), len(tau_rows), min(taus),
                 sum(taus) / len(taus)))
    else:
        print('   Kendall tau: not computable on the sample')
    print('   samples (row, pos_swaps, set_divergence, tau):')
    for i, mm, sd, tau in samples:
        print('     (%d, %d, %d, %s)'
              % (i, mm, sd, 'n/a' if tau is None else '%.6f' % tau))
    print('   RESULT: DIVERGENT (%d rows differ)' % len(diff))
    return 1


if __name__ == '__main__':
    sys.exit(main())