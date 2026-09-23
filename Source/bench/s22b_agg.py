#!/usr/bin/env python3
"""S2.2b aggregator: timing (ABBA), memory gates (6 builds), drift, REPRU.

Reads Data/phase_profile/etapa_b/s22b/{t1,t2,t12,tmem}/raw/*.txt and prints
the tables the S2.2b README is built from:

  TIMING  rep-1 totals per sample (warm_* excluded; rep 0 discarded), the
          median of the 3 samples per builder/T and legacy->v2 delta in %.
  PHASES  rep-1 PHASEPROFILE total_ms medians per builder/T. DRVAGG lines
          are NOT used: the driver aggregates them over ALL reps, so they
          would silently re-include rep 0.
  DRIFT   canary_0 vs canary_1 (session drift bound per T).
  REPRU   minflt rep0 vs rep1 (mechanism evidence for the rep-0 penalty).
  MEM     6 builds x 2 conventions:
            conv1 = DONE hwm of the TIMING captures (true process peak --
                    clear_refs never ran there);
            conv2 = per-rep SUM of MEMBUILD delta_hwm_kb in the PHASE_MEM
                    captures (isolated build raise after the reset);
          gate PASS when v2 <= legacy per T on BOTH conventions;
          residue = d(conv1) - d(conv2) printed WITH SIGN: negative is
          legitimate, because conv1 also carries the emission working set
          (legacy temp_triplets_) while conv2 only measures the raise
          inside build_from_* -- the phases are not additive, so the
          residual must not be forced to zero.

Usage: python3 Source/bench/s22b_agg.py [root]
"""
import glob
import os
import statistics as st
import sys

BUILDERS = ('legacy', 'v2')
TS = ('1', '2', '12')


def parse(path):
    reps, phases, repru, membuild = {}, {}, {}, []
    cur = None
    hwm = None
    with open(path) as f:
        for line in f:
            p = line.rstrip('\n').split(',')
            if not p:
                continue
            if p[0] == 'REP':
                cur = int(p[1])
                reps[cur] = float(p[2])
            elif p[0] == 'REPRU':
                repru[int(p[1])] = (int(p[2]), int(p[3]), int(p[4]))
            elif p[0] == 'PHASEPROFILE' and len(p) > 2 and p[1] == 'MEMBUILD':
                d = {}
                for kv in p[2:]:
                    if '=' in kv:
                        k, v = kv.split('=', 1)
                        d[k] = int(v) if v.lstrip('-').isdigit() else v
                d['_rep'] = cur
                membuild.append(d)
            elif p[0] == 'PHASEPROFILE' and cur == 1 and len(p) > 2:
                for kv in p[2:]:
                    if kv.startswith('total_ms='):
                        phases[p[1]] = float(kv[len('total_ms='):])
            elif p[0] == 'DONE' and len(p) > 2:
                try:
                    hwm = int(p[2])
                except ValueError:
                    hwm = None
    return dict(reps=reps, phases=phases, repru=repru,
                membuild=membuild, hwm=hwm)


def samples(raw, b):
    out = []
    for path in sorted(glob.glob(os.path.join(raw, 'p*_%s.txt' % b))):
        rec = parse(path)
        if 1 in rec['reps']:
            out.append(rec)
    return out


def med(xs):
    return st.median(xs) if xs else float('nan')


def load(root):
    all_s = {}
    for T in TS:
        for b in BUILDERS:
            all_s[(T, b)] = samples(os.path.join(root, 't' + T, 'raw'), b)
    return all_s


def report_timing(all_s):
    print('=== TIMING (rep 1 only, warm excluded, ABBA pairs) ===')
    for T in TS:
        med_t = {}
        for b in BUILDERS:
            xs = [s['reps'][1] for s in all_s[(T, b)]]
            med_t[b] = med(xs)
            print('  T=%-2s %-6s samples_ms=%s median_ms=%.1f'
                  % (T, b, ['%.1f' % x for x in xs], med_t[b]))
        d = med_t['legacy'] - med_t['v2']
        print('  T=%-2s delta legacy-v2 = %+.1f ms (%+.2f%% of legacy)'
              % (T, d, 100.0 * d / med_t['legacy']))


# --- report sections + main (part 2) ---
def report_phases(all_s):
    print('=== PHASES (rep 1 only, median ms over 3 samples) ===')
    for T in TS:
        names = set()
        for b in BUILDERS:
            for s in all_s[(T, b)]:
                names |= set(s['phases'])
        print('  -- T=%s' % T)
        for nm in sorted(names):
            v = {b: med([s['phases'][nm] for s in all_s[(T, b)]
                         if nm in s['phases']]) for b in BUILDERS}
            ratio = '%.2fx' % (v['legacy'] / v['v2']) if v['v2'] else 'n/a'
            print('    %-22s legacy=%9.3f  v2=%9.3f  ratio=%s'
                  % (nm, v['legacy'], v['v2'], ratio))


def report_drift(root):
    print('=== DRIFT (canary w1, rep 1 totals) ===')
    for T in TS:
        vals = []
        for i in (0, 1):
            p = os.path.join(root, 't' + T, 'raw', 'canary_%d.txt' % i)
            if os.path.isfile(p):
                vals.append(parse(p)['reps'].get(1))
        if len(vals) == 2 and None not in vals:
            a, b_ = vals
            print('  T=%-2s canary_0=%.1f canary_1=%.1f drift=%+.2f%%'
                  % (T, a, b_, 100.0 * (b_ - a) / a))
        else:
            print('  T=%-2s canary missing' % T)


def report_repru(all_s):
    print('=== REPRU (minflt median over captures) ===')
    for T in TS:
        for b in BUILDERS:
            r0 = med([s['repru'][0][0] for s in all_s[(T, b)]
                      if 0 in s['repru']])
            r1 = med([s['repru'][1][0] for s in all_s[(T, b)]
                      if 1 in s['repru']])
            print('  T=%-2s %-6s rep0=%.0f rep1=%.0f (%.2fx)'
                  % (T, b, r0, r1, r0 / r1 if r1 else float('nan')))


def mem_conv2(root, T, b):
    p = os.path.join(root, 'tmem', 'raw', 'mem_t%s_%s.txt' % (T, b))
    if not os.path.isfile(p):
        return float('nan')
    per_rep = {}
    for mb in parse(p)['membuild']:
        if mb.get('_rep') is not None:
            per_rep[mb['_rep']] = per_rep.get(mb['_rep'], 0) + \
                mb['delta_hwm_kb']
    return med(list(per_rep.values()))


def report_mem(all_s, root):
    print('=== MEM: 6 builds, 2 conventions ===')
    c1, c2 = {}, {}
    for T in TS:
        for b in BUILDERS:
            c1[(T, b)] = med([s['hwm'] for s in all_s[(T, b)]
                              if s['hwm'] and s['hwm'] > 0])
            c2[(T, b)] = mem_conv2(root, T, b)
    for T in TS:
        d1 = c1[(T, 'legacy')] - c1[(T, 'v2')]
        d2 = c2[(T, 'legacy')] - c2[(T, 'v2')]
        gate = 'PASS' if (c1[(T, 'v2')] <= c1[(T, 'legacy')]
                          and c2[(T, 'v2')] <= c2[(T, 'legacy')]) else 'FAIL'
        print('  T=%-2s conv1_peak  legacy=%.0f v2=%.0f d=%+.0f kB (%+.1f%%)'
              % (T, c1[(T, 'legacy')], c1[(T, 'v2')], d1,
                 100.0 * d1 / c1[(T, 'legacy')]))
        print('  T=%-2s conv2_build legacy=%.0f v2=%.0f d=%+.0f kB (%+.1f%%)'
              % (T, c2[(T, 'legacy')], c2[(T, 'v2')], d2,
                 100.0 * d2 / c2[(T, 'legacy')]))
        print('  T=%-2s residue(conv1-conv2)=%+.0f kB  gate=%s'
              % (T, d1 - d2, gate))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(here)),
        'Data', 'phase_profile', 'etapa_b', 's22b')
    if not os.path.isdir(os.path.join(root, 't1', 'raw')):
        print('ERROR: no captures under ' + root)
        return 2
    all_s = load(root)
    report_timing(all_s)
    report_phases(all_s)
    report_drift(root)
    report_repru(all_s)
    report_mem(all_s, root)
    return 0


if __name__ == '__main__':
    sys.exit(main())