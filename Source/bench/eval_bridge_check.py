#!/usr/bin/env python3
"""eval_bridge_check.py -- compares the REAL UDLF evaluation module's log
(Evaluation.hpp via Source/tests/eval_bridge.cpp) against rfe_quality.py's
metrics on the SAME rank-list files.

Usage:
  python3 Source/bench/eval_bridge_check.py --log bridge_eval.log \
      --in rk_in.txt --leg rk_out.txt --classes CLASSES [--tol 1e-3]

The bridge log contains exactly two EFFECTIVENESS blocks (Logger's format):
  block 0 = "before" (the --in file), block 1 = "after" (the --leg file).
Metric lines look like "\\nP@5\\t49.652%" / "\\nRecall@20\\t6.02995%" and
"\\n MAP\\t10.9619" (MAP at k = dataProvider.getL(), i.e. MAP@L).

Exit: 0 = all metrics agree within --tol, 1 = disagreement, 2 = usage/data.
"""
import argparse
import re
import subprocess
import sys

TOL_DEFAULT = 1e-3  # the Logger prints ~6 significant digits


def parse_blocks(log_text):
    blocks = re.findall(r'EFFECTIVENESS\n(.*?)(?:\n\n|\Z)', log_text, re.S)
    if len(blocks) != 2:
        print('ERROR: expected 2 EFFECTIVENESS blocks, got %d' % len(blocks))
        sys.exit(2)
    out = []
    for b in blocks:
        m = {}
        for kind, k, val in re.findall(r'\n (P|Recall)@(\d+)\t([\d.]+)%', b):
            m['%s@%s' % (kind, k)] = float(val)
        mp = re.search(r'\n MAP\t([\d.]+)', b)
        if mp:
            m['MAP'] = float(mp.group(1))  # cutoff = L (unknown here)
        out.append(m)
    return out


def harness_metrics(rk, classes, ks, map_ks):
    cmd = [sys.executable, 'Source/bench/rfe_quality.py', 'metrics',
           '--rk', rk, '--classes', classes, '--ks', ks, '--map-ks', map_ks]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('ERROR: rfe_quality.py failed:\n%s%s' % (p.stdout, p.stderr))
        sys.exit(2)
    return {name: float(val) for name, val in
            re.findall(r'METRIC,([^,]+),([\d.]+)', p.stdout)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--log', required=True)
    ap.add_argument('--in', dest='rin', required=True, help='input rk (before)')
    ap.add_argument('--leg', required=True, help='legacy rk (after)')
    ap.add_argument('--classes', required=True)
    ap.add_argument('--L', type=int, required=True)
    ap.add_argument('--ks', default='5,10,20,50')
    ap.add_argument('--tol', type=float, default=TOL_DEFAULT)
    args = ap.parse_args()

    blocks = parse_blocks(open(args.log).read())
    h_in = harness_metrics(args.rin, args.classes, args.ks, str(args.L))
    h_leg = harness_metrics(args.leg, args.classes, args.ks, str(args.L))
    # The module logs a single MAP at k=L with no suffix; normalize the
    # harness's MAP@L to the same key so each metric is compared exactly once.
    for h in (h_in, h_leg):
        key = 'MAP@%d' % args.L
        if key in h:
            h['MAP'] = h.pop(key)

    fails = 0
    # P/Recall: keyed exactly. MAP: module logs MAP@L -> harness MAP@L.
    for label, mod, har in (('input', blocks[0], h_in),
                            ('legacy', blocks[1], h_leg)):
        for key in sorted(set(mod) | set(har)):
            hval = har.get(key)
            mval = mod.get(key)
            if hval is None or mval is None:
                print('BRIDGE,%s,%s,MISSING module=%s harness=%s,FAIL'
                      % (label, key, mval, hval))
                fails += 1
                continue
            d = abs(mval - hval)
            ok = d <= args.tol
            print('BRIDGE,%s,%s,module=%.6f,harness=%.6f,diff=%.2g,%s'
                  % (label, key, mval, hval, d, 'OK' if ok else 'FAIL'))
            fails += (not ok)
    print('BRIDGEVERDICT,%s,tol=%g'
          % ('PASS' if fails == 0 else 'FAIL fails=%d' % fails, args.tol))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
