#!/usr/bin/env python3
"""Item 4: bind-matrix aggregation over /tmp/bindm captures.

Medians over reps 1..n (rep 0 discarded) of the wall clock and, when present,
the per-stage ledger (DRVAGG medians, all reps). Prints a compact table.
"""
import glob
import os
import statistics as st

rows = {}
for f in sorted(glob.glob('/tmp/bindm/*.txt')):
    name = os.path.basename(f)[:-4]          # e.g. head_b1_real
    kernel, bind, wl = name.split('_', 2)
    bind = bind[1:]                          # b0 -> 0
    reps, led = [], {}
    for line in open(f):
        p = line.rstrip('\n').split(',')
        if p[0] == 'REP':
            reps.append(float(p[2]))
        elif p[0] == 'DRVAGG':
            led[p[1]] = float(p[2].split('=')[1])
    tset = ''
    for line in open(f):
        if line.startswith('TSET,'):
            for kv in line.strip().split(',')[1:]:
                if kv.startswith('max_concurrency='):
                    tset = kv.split('=')[1]
            break
    med = st.median(reps[1:] or reps)
    rows[(kernel, bind, wl)] = dict(med=med, tset=tset, led=led)

print(f'{"kernel":<7}{"bind":>5}{"wl":<6}{"med_ms":>10}{"tbb_max":>9}'
      f'{"pos_build":>11}{"H_build":>9}{"mult_S":>9}{"mult_Srev":>11}')
for k in ('base', 'head'):
    for b in ('0', '1'):
        for wl in ('real', 'w1'):
            r = rows.get((k, b, wl))
            if not r:
                continue
            g = lambda key: r['led'].get(key, float('nan'))
            print(f'{k:<7}{b:>5}{wl:<6}{r["med"]:>10.1f}{r["tset"]:>9}'
                  f'{g("position_build"):>11.1f}{g("H_build"):>9.1f}'
                  f'{g("mult_S"):>9.1f}{g("mult_Srev"):>11.1f}')
