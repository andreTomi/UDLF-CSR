#!/usr/bin/env python3
"""S1: same-session interleaved A/B (base vs HEAD, real ALOI LHRR @12t).

Reads /tmp/s1_ab: 6 interleaved blocks x 2 reps (base, head, head, base, base,
head), two canary runs (fixed w1 load, before/after), one cold-base run after a
180 s idle cooldown. Median over each block's 2 reps; paired ratio by block
position; canary drift quantifies session stability.
"""
import os
import statistics as st

D = '/tmp/s1_ab'


def reps_of(path):
    return [float(l.split(',')[2]) for l in open(path) if l.startswith('REP,')]


def nnz_chk(path):
    for l in open(path):
        if l.startswith('REP,'):
            return l.split(',')[3], l.split(',')[4]
    return None, None


base, head = {}, {}
for f in sorted(os.listdir(D)):
    p = os.path.join(D, f)
    if f.startswith('base_blk'):
        base[int(f.split('blk')[1][0])] = reps_of(p)
    elif f.startswith('head_blk'):
        head[int(f.split('blk')[1][0])] = reps_of(p)

pos = [(1, 'base'), (2, 'head'), (3, 'head'), (4, 'base'), (5, 'base'), (6, 'head')]
print('== blocks (median of the 2 reps of each block) ==')
print(f'{"pos":>3} {"kernel":<6} {"blk_ms":>10} {"rep1":>9} {"rep2":>9}')
seq = []
for b, k in pos:
    r = base[b] if k == 'base' else head[b]
    m = st.median(r)
    seq.append((k, m))
    print(f'{b:>3} {k:<6} {m:>10.1f} {r[0]:>9.1f} {r[1]:>9.1f}')

bm = st.median([m for k, m in seq if k == 'base'])
hm = st.median([m for k, m in seq if k == 'head'])
print(f'\n== global: base {bm:.1f} ms | head {hm:.1f} ms | '
      f'head/base = {bm/hm:.3f}x ==')

# paired by block position: head vs its adjacent base blocks
pairs = []
for i, (k, m) in enumerate(seq):
    if k != 'head':
        continue
    prev_b = next((m2 for kk, m2 in reversed(seq[:i]) if kk == 'base'), None)
    next_b = next((m2 for kk, m2 in seq[i + 1:] if kk == 'base'), None)
    adj = [x for x in (prev_b, next_b) if x is not None]
    if adj:
        pairs.append(st.median(adj) / m)
print('== paired ratios (adjacent base/head) ==')
print('  '.join(f'{p:.3f}' for p in pairs) + f'   median = {st.median(pairs):.3f}x')

c0 = st.median(reps_of(os.path.join(D, 'canary_0.txt')))
c1 = st.median(reps_of(os.path.join(D, 'canary_1.txt')))
cb = st.median(reps_of(os.path.join(D, 'base_cold.txt')))
print('\n== session stability (canary = fixed w1 load, same binary) ==')
print(f'canary before {c0:.1f} ms | after {c1:.1f} ms | drift {100.0*(c1-c0)/c0:+.1f}%')
b1 = st.median(base[1]); b4 = st.median(base[4]); b5 = st.median(base[5])
print('base blocks over time: ' + '  '.join(f'{x:.1f}' for x in (b1, b4, b5)))
print(f'cold-base (after 180s idle): {cb:.1f} ms | vs warm base median {bm:.1f} '
      f'({100.0*(cb-bm)/bm:+.1f}%)')

n, c = nnz_chk(os.path.join(D, 'head_blk2.txt'))
n2, c2 = nnz_chk(os.path.join(D, 'base_blk1.txt'))
print(f'\n== correctness: head nnz={n} chk={c[:14]}... | base nnz={n2} '
      f'chk={c2[:14]}... | {"MATCH" if (n, c) == (n2, c2) else "MISMATCH"}')
