#!/usr/bin/env python3
"""Aggregate the A/B: per-variant/workload median of reps 1..n (rep 0 discarded).

Both the wall clock (REP lines) and each PHASEPROFILE phase are reduced with the
same rule: drop rep 0 (warmup), take the median of the remaining reps. Blocks are
delimited by the REP line that starts each repetition.
"""
import glob, os, statistics as st, sys

DIR = sys.argv[1] if len(sys.argv) > 1 else '/tmp/ab'
PHASES = ['F1_ACCUM', 'F2_PRUNE', 'MP_CALL', 'BUILD_CALL']
FIELDS = {p: [] for p in PHASES}
WALL = 'wall'


def flush(block, acc):
    """Append one repetition's phase values to the per-phase sample lists."""
    if not block:
        return
    for p in PHASES:
        if p in block:
            acc.setdefault(p, []).append(block[p])


rows = {}
for f in sorted(glob.glob(os.path.join(DIR, '*.txt'))):
    base = os.path.basename(f)[:-4]
    if base == 'summary' or '_' not in base:
        continue                      # skip the aggregator's own output
    variant, w = base.split('_', 1)
    reps, checks, nnz = [], set(), set()
    blocks, block = [], {}
    hwm = None
    for line in open(f):
        p = line.rstrip('\n').split(',')
        if p[0] == 'REP':
            blocks.append(block)
            block = {}
            reps.append(float(p[2])); nnz.add(p[3]); checks.add(p[4])
        elif p[0] == 'PHASEPROFILE':
            name = p[1]
            if name in PHASES:
                block[name] = float(p[2].split('=')[1])
            elif name == 'HWM':
                hwm = int(p[5].split('=')[1])
    blocks.append(block)
    blocks = blocks[1:]                      # drop the pre-REP empty block
    acc = {p: [] for p in PHASES}
    for b in blocks[1:]:                     # rep 0 = warmup, discarded
        flush(b, acc)
    med = lambda v: st.median(v) if v else float('nan')
    rows[(variant, w)] = dict(n=len(reps), med=med(reps[1:] or reps),
                              lo=min(reps[1:] or reps), hi=max(reps[1:] or reps),
                              phases={p: med(acc[p]) for p in PHASES},
                              hwm=hwm, nnz=sorted(nnz), chk=sorted(checks))

order = ['w1', 'w2', 'w3', 'w4']
variants = ['base', 'f1', 'f13', 'f3b', 'f1b', 'fin']
print(f'{"wl":<4}{"var":<6}{"n":>3}{"median_ms":>12}{"min":>10}{"max":>10}{"spread%":>9}'
      f'{"F1_ACC":>10}{"F2_PRUN":>10}{"MP_CALL":>10}{"BUILD":>9}{"HWM_MB":>9}  checksum')
for w in order:
    for v in variants:
        r = rows.get((v, w))
        if not r: continue
        spread = 100.0 * (r['hi'] - r['lo']) / r['med'] if r['med'] else 0
        ph = r['phases']
        print(f'{w:<4}{v:<6}{r["n"]:>3}{r["med"]:>12.1f}{r["lo"]:>10.1f}{r["hi"]:>10.1f}'
              f'{spread:>9.1f}{ph["F1_ACCUM"]:>10.1f}{ph["F2_PRUNE"]:>10.1f}'
              f'{ph["MP_CALL"]:>10.1f}{ph["BUILD_CALL"]:>9.1f}'
              f'{r["hwm"]/1024:>9.1f}  {r["chk"][0]}')
    print()
print('(all columns except HWM/checksum are medians over reps 1..n; rep 0 discarded)')

