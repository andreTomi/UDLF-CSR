#!/usr/bin/env python3
"""A5-bis memory ledger: per-build decomposition of the real (ALOI) captures.

Usage: python3 Source/bench/a5bis_mem.py <dir> [...]

For every capture in <dir> prints:
  * final VmHWM / ru_maxrss from the DONE line (the true process peak);
  * per build index n (1 = position matrix, 2 = H, 3..6 = mult/combine outputs)
    the median over reps of emitted / tmp_bytes / out_bytes / alloc_bytes /
    delta_hwm_kb / bytes_per_triplet, as emitted by `PHASEPROFILE,MEMBUILD`
    (PHASE_MEM=1, with /proc/self/clear_refs=5 reset before each build).

tmp_bytes > 0 marks a build that still routes through the GLOBAL builder
(`build_from_partial_results`: temp_triplets_ + global sort); tmp_bytes == 0
marks the fragment-CSR path (Fase 1), whose cost is just the CSR output.
"""
import glob
import os
import statistics as st
import sys

FIELDS = ['emitted', 'tmp_bytes', 'out_bytes', 'alloc_bytes', 'delta_hwm_kb',
          'bytes_per_triplet']


def parse(path):
    per_build = {}
    hwm = None
    done = None
    for line in open(path):
        if line.startswith('PHASEPROFILE,MEMBUILD'):
            kv = dict(p.split('=', 1) for p in line.strip().split(',')[2:])
            n = int(kv['n'])
            per_build.setdefault(n, {k: [] for k in FIELDS})
            for k in FIELDS:
                if k in kv:
                    per_build[n][k].append(float(kv[k]))
        elif line.startswith('PHASEPROFILE,HWM'):
            hwm = dict(p.split('=', 1) for p in line.strip().split(',')[2:])
        elif line.startswith('DONE'):
            p = line.strip().split(',')
            done = (int(p[2]), int(p[3]))          # vmhwm_kb, ru_maxrss_kb
    med = lambda v: st.median(v) if v else float('nan')
    return dict(tag=os.path.basename(path)[:-4], hwm=hwm, done=done,
                builds={n: {k: med(v) for k, v in d.items()}
                        for n, d in per_build.items()})


for d in sys.argv[1:]:
    for f in sorted(glob.glob(os.path.join(d, '*.txt'))):
        if os.path.basename(f).startswith('extract'):
            continue
        r = parse(f)
        peak = (r['done'][0] / 1024) if r['done'] else float('nan')
        print(f"== {r['tag']}: VmHWM final {peak:.0f} MB "
              f"(ru_maxrss {r['done'][1]/1024:.0f} MB)")
        print(f'{"n":>2}{"emitted":>12}{"tmp_B":>13}{"out_B":>13}'
              f'{"alloc_B":>13}{"dHWM_MB":>10}{"B/tripl":>9}  path')
        for n in sorted(r['builds']):
            b = r['builds'][n]
            path = 'GLOBAL' if b['tmp_bytes'] > 0 else 'frag-CSR'
            print(f'{n:>2}{b["emitted"]:>12.0f}{b["tmp_bytes"]:>13.0f}'
                  f'{b["out_bytes"]:>13.0f}{b["alloc_bytes"]:>13.0f}'
                  f'{b["delta_hwm_kb"]/1024:>10.1f}{b["bytes_per_triplet"]:>9.1f}'
                  f'  {path}')
        print()
