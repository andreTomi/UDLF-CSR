#!/usr/bin/env python3
"""S1 / S1-bis aggregation with the permanent A/B policy.

Policy (pre-registered, applied to every A/B from now on):
  (1) the FIRST block of a session is discarded   -- cold .bin / cold pages;
  (2) rep 0 of every block is discarded           -- first-touch of a fresh
      process (the driver runs reps in-process; see the REPRU evidence).
Both are reported explicitly, and the "all data" numbers are still printed so
the effect of the policy is visible instead of hidden.

Handles the S1 raw layout (base_blk1..base_blk5 / head_blk2..head_blk6 /
base_cold / canary_*) and the S1-bis layout (b1_base..b6_head / canary_*).

Usage: python3 Source/bench/s1_bis_agg.py <dir> [<dir2> ...]
"""
import os
import re
import statistics as st
import sys

SEQ = ['base', 'head', 'head', 'base', 'base', 'head']   # block order, both runs


def read_file(path):
    reps, ru = [], []
    for line in open(path):
        p = line.rstrip('\n').split(',')
        if line.startswith('REP,'):
            reps.append(float(p[2]))
        elif line.startswith('REPRU,'):
            ru.append((int(p[2]), int(p[3]), int(p[4])))
    return reps, ru


def load(d):
    raw = os.path.join(d, 'raw')
    if not os.path.isdir(raw):          # S1 layout: files sit in the dir itself
        raw = d
    blocks, canaries, cold = {}, {}, None
    for fn in sorted(os.listdir(raw)):
        tag = fn[:-4] if fn.endswith('.txt') else fn
        reps, ru = read_file(os.path.join(raw, fn))
        if not reps:
            continue
        if tag.startswith('canary'):
            canaries[tag] = reps
            continue
        if 'cold' in tag:
            cold = (tag, reps, ru)
            continue
        m = re.match(r'^base_blk(\d)$', tag)
        m2 = re.match(r'^head_blk(\d)$', tag)
        m3 = re.match(r'^b(\d)_(base|head)$', tag)
        if m:
            pos, kern = int(m.group(1)), 'base'
        elif m2:
            pos, kern = int(m2.group(1)), 'head'
        elif m3:
            pos, kern = int(m3.group(1)), m3.group(2)
        else:
            continue
        blocks[pos] = (kern, reps, ru)
    return blocks, canaries, cold


def report(d):
    blocks, canaries, cold = load(d)
    if not blocks:
        print(f'{d}: no blocks found')
        return
    order = sorted(blocks)
    print(f'\n===== {d} =====')
    print(f'{"pos":>3} {"kernel":<6} {"rep0":>9} {"rep1":>9} {"med_all":>9} '
          f'{"minflt0":>9} {"minflt1":>9}')
    for pos, kern in zip(order, SEQ[:len(order)]):
        k, reps, ru = blocks[pos]
        f0 = ru[0][0] if ru else -1
        f1 = ru[1][0] if len(ru) > 1 else -1
        flag = '' if k == kern else '  <-- sequence mismatch'
        print(f'{pos:>3} {k:<6} {reps[0]:>9.1f} {reps[1]:>9.1f} '
              f'{st.median(reps):>9.1f} {f0:>9} {f1:>9}{flag}')

    def med(kern, use_rep1, skip_first):
        vals = []
        for pos in order:
            k, reps, _ = blocks[pos]
            if k != kern:
                continue
            if skip_first and pos == order[0]:
                continue
            vals.append(reps[-1] if use_rep1 else st.median(reps))
        return st.median(vals) if vals else float('nan')

    print('\n-- ratios (head/base) under the policy settings --')
    for use_rep1 in (False, True):
        for skip_first in (False, True):
            b, h = med('base', use_rep1, skip_first), med('head', use_rep1, skip_first)
            tag = ('rep1-only' if use_rep1 else 'all-reps') + \
                  (' +first-block-dropped' if skip_first else '')
            print(f'  {tag:<32} base {b:>8.1f}  head {h:>8.1f}  '
                  f'ratio {b / h:>5.3f}x')

    print('\n-- paired ratios, head vs the ADJACENT base blocks (first block dropped) --')
    used = [p for p in order if p != order[0]]
    seq = [(blocks[p][0], (blocks[p][1][-1] if len(blocks[p][1]) > 1
                           else st.median(blocks[p][1]))) for p in used]
    pairs = []
    for i, (k, v) in enumerate(seq):
        if k != 'head':
            continue
        prev_b = next((m for kk, m in reversed(seq[:i]) if kk == 'base'), None)
        next_b = next((m for kk, m in seq[i + 1:] if kk == 'base'), None)
        adj = [x for x in (prev_b, next_b) if x is not None]
        if adj:
            pairs.append(st.median(adj) / v)
    if pairs:
        print('  ' + '  '.join(f'{p:.3f}' for p in pairs) +
              f'   median = {st.median(pairs):.3f}x   (n={len(pairs)} pairs)')

    if len(canaries) >= 2:
        ks = sorted(canaries)
        c0, c1 = st.median(canaries[ks[0]]), st.median(canaries[ks[-1]])
        print(f'\n-- canary (fixed load): {c0:.1f} -> {c1:.1f} ms '
              f'({100.0 * (c1 - c0) / c0:+.1f}% drift)')
    if cold:
        tag, reps, ru = cold
        b_all = med('base', False, False)
        b_rep1 = med('base', True, True)
        print(f'-- cold-start probe ({tag}): {st.median(reps):.1f} ms '
              f'vs warm base {b_all:.1f} all-reps '
              f'({100.0 * (st.median(reps) - b_all) / b_all:+.1f}%), '
              f'{b_rep1:.1f} rep1-only (policy)')


if __name__ == '__main__':
    dirs = sys.argv[1:] or ['/tmp/s1_bis']
    for d in dirs:
        report(d)
