#!/usr/bin/env python3
"""P@k / Recall / MAP replica of Evaluation.hpp + Baseline gate (S2.2a).

Replicates the exact semantics of the published evaluator:

  PrecisionStrategy / RecallStrategy / MAPStrategy (Evaluation.hpp:29-110)
  class ids (DataLoader.hpp:84-100): imageIds[i] = sequential id of the class
    name after the FIRST ':' on classes.txt line i, assigned on first
    appearance; classSizes = per-class image counts.
  rk arrangement (VirtualArray.hpp:228-242 processLine with the (n, n) call
    of Evaluation.hpp:125/129): rank j of query i = token j of line i at
    rowOffset i*n; input lines hold 7200 tokens, every requested k <= 2000.

Numeric model: P@k = float32(int)/float32(int) (exact, counts < 2^24);
Recall and MAP accumulate in float32, row by row, in file order -- mirrored
with numpy float32 ops (vectorised division, sequential accumulation loops).

Usage:
  python3 Source/bench/pk_eval.py gate [--dataset aloi] [--repo ROOT]
      Evaluate Datasets/<ds>/rkLists.txt and require every Baseline column of
      Data/Unified_Accuracy_Results.csv:
        P@k         exact at the CSV's printed precision (integer count)
        Recall/MAP  within --tol-units print units (default 2)
  python3 Source/bench/pk_eval.py eval --rk FILE [--bin] --classes FILE
      [--L L] --ks 4,10 --map-ks 100,400
      Same metrics on an arbitrary rank list: text rkLists, or the raw u32
      N*L binary the driver writes with PHASE_RK_OUT (with --bin --L).

Exit: 0 pass, 1 gate mismatch, 2 usage/data error.
"""
import argparse
import csv
import os
import sys

import numpy as np


def die(msg):
    print('ERROR: ' + msg, file=sys.stderr)
    sys.exit(2)


def load_classes(path):
    """DataLoader.hpp:60-100: class name = after the first ':'; id = order
    of first appearance; classSizes = per-id counts."""
    names = []
    with open(path, 'r') as f:
        for ln in f:
            p = ln.find(':')
            if p < 0:
                die('classes line without colon: %r' % ln[:40])
            names.append(ln[p + 1:])
    cid = {}
    ids = np.empty(len(names), dtype=np.uint32)
    for i, nm in enumerate(names):
        j = cid.get(nm)
        if j is None:
            j = len(cid)
            cid[nm] = j
        ids[i] = j
    csize = np.bincount(ids, minlength=len(cid)).astype(np.uint32)
    return ids, csize


def load_rk_text(path, N, kmax):
    """First kmax tokens of each of the first N lines (Evaluation reads at
    most k <= kmax per row; processLine stops at end of line)."""
    rk = np.empty((N, kmax), dtype=np.uint32)
    with open(path, 'r') as f:
        for i in range(N):
            line = f.readline()
            if not line:
                die('%s: fewer than %d lines' % (path, N))
            parts = line.split(None, kmax)
            if len(parts) < kmax:
                die('%s line %d: %d tokens, need %d'
                    % (path, i, len(parts), kmax))
            try:
                rk[i] = np.array(parts[:kmax], dtype=np.uint32)
            except ValueError:
                die('%s line %d: non-numeric token' % (path, i))
    return rk


def load_rk_bin(path, N, L):
    rk = np.fromfile(path, dtype=np.uint32)
    if rk.size != N * L:
        die('%s: %d u32, expected N*L=%d' % (path, rk.size, N * L))
    return rk.reshape(N, L)


def compute_metrics(rk, image_ids, csize, ks_p, ks_r, ks_m):
    """{metric name: percent} mirroring Evaluation.hpp's three strategies."""
    N = len(image_ids)
    kmax = max(ks_p + ks_r + ks_m)
    if rk.shape[1] < kmax:
        die('rank lists have L=%d < requested k=%d' % (rk.shape[1], kmax))
    sub = rk[:, :kmax]
    # same[i, j]: the image retrieved at (i, j) shares query i's class
    same = image_ids[sub] == image_ids[:, None]
    out = {}
    f32 = np.float32

    # P@k (Evaluation.hpp:29-52): f32(totalCorrect) / f32(n*k)
    for k in ks_p:
        hits = int(same[:, :k].sum())
        out['P@%d' % k] = float(f32(hits) / f32(N * k)) * 100.0

    # Recall@k (:54-80): per row f32(hits)/f32(classSize), sequential f32 sum
    rel32 = csize[image_ids].astype(f32)  # relevantTotal of each query's class
    for k in ks_r:
        terms = same[:, :k].sum(axis=1).astype(f32) / rel32
        acc = f32(0)
        for t in terms:            # sequential += in file order (C++ loop)
            acc += t
        out['Recall@%d' % k] = float(acc / f32(N)) * 100.0

    # MAP@k (:82-110): term_j = f32(cum_j)/f32(j+1) on hits only (adding 0f
    # on misses is exact), f32 row cumsum, /f32(min(k, classSize)), f32 sum.
    for k in ks_m:
        sk = same[:, :k]
        cum = sk.cumsum(axis=1, dtype=np.uint32)
        den = np.arange(1, k + 1, dtype=f32)
        terms = np.where(sk, cum.astype(f32) / den, f32(0))
        row_sum = terms.cumsum(axis=1, dtype=f32)[:, -1]
        denom = np.minimum(np.uint32(k), csize[image_ids]).astype(f32)
        acc = f32(0)
        for t in row_sum / denom:
            acc += t
        out['MAP@%d' % k] = float(acc / f32(N)) * 100.0
    return out

# --- gate against the published CSV (S2.2a acceptance) ---

def parse_metric_cols(header):
    """Wide-CSV columns -> [(prefix, k, column)] for P@k/Recall@k/MAP@k."""
    want = []
    for col in header:
        for pre in ('P@', 'Recall@', 'MAP@'):
            if col.startswith(pre) and col[len(pre):].isdigit():
                want.append((pre, int(col[len(pre):]), col))
    return want


def gate(args):
    repo = args.repo or os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ds = args.dataset
    rk_path = os.path.join(repo, 'Datasets', ds, 'rkLists.txt')
    cls_path = os.path.join(repo, 'Datasets', ds, 'classes.txt')
    csv_path = os.path.join(repo, 'Data', 'Unified_Accuracy_Results.csv')
    for p in (rk_path, cls_path, csv_path):
        if not os.path.isfile(p):
            die('missing file: ' + p)
    with open(csv_path, newline='') as f:
        rows = list(csv.DictReader(f))
    base = None
    for r in rows:
        if r.get('Dataset') == ds and r.get('Approach') == 'Baseline':
            base = r
    if base is None:
        die('no Baseline row for dataset %s in %s' % (ds, csv_path))
    want = [(pre, k, col) for pre, k, col in parse_metric_cols(list(base.keys()))
            if (base.get(col) or '').strip()]
    if not want:
        die('Baseline row has no metric columns')
    ks_p = sorted({k for pre, k, _ in want if pre == 'P@'})
    ks_r = sorted({k for pre, k, _ in want if pre == 'Recall@'})
    ks_m = sorted({k for pre, k, _ in want if pre == 'MAP@'})
    kmax = max(ks_p + ks_r + ks_m)

    image_ids, csize = load_classes(cls_path)
    N = len(image_ids)
    print('GATE,dataset=%s,N=%d,kmax=%d,columns=%d,tol_units_recmap=%.1f'
          % (ds, N, kmax, len(want), args.tol_units))
    rk = load_rk_text(rk_path, N, kmax)
    got = compute_metrics(rk, image_ids, csize, ks_p, ks_r, ks_m)

    ok = True
    for pre, k, col in want:
        exp_s = (base[col] or '').strip()
        exp = float(exp_s)
        dec = len(exp_s.split('.')[1]) if '.' in exp_s else 0
        unit = 10.0 ** (-dec)
        g = got[col]
        diff = abs(g - exp)
        # P@k: exact at print precision (integer count gate). Recall/MAP:
        # float32 accumulators -- tolerance in units of the published digits.
        tol = (0.5 if pre == 'P@' else args.tol_units) * unit
        passed = diff <= tol + 1e-12
        ok = ok and passed
        print('GATE,%s,expect=%s,got=%.6f,diff=%.3g,units=%.2f,%s'
              % (col, exp_s, g, diff, diff / unit,
                 'PASS' if passed else 'FAIL'))
    print('GATE,' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def ev(args):
    image_ids, csize = load_classes(args.classes)
    N = len(image_ids)
    ks = sorted({int(x) for x in args.ks.split(',') if x.strip()})
    map_ks = sorted({int(x) for x in (args.map_ks or '').split(',')
                     if x.strip()})
    kmax = max(ks + map_ks)
    if args.bin:
        if args.L <= 0:
            die('--bin requires --L (row length of the u32 binary)')
        rk = load_rk_bin(args.rk, N, args.L)
    else:
        rk = load_rk_text(args.rk, N, kmax)
    got = compute_metrics(rk, image_ids, csize, ks, ks, map_ks)
    for name in sorted(got, key=lambda s: (s.split('@')[0],
                                           int(s.split('@')[1]))):
        print('METRIC,%s,%.6f' % (name, got[name]))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description='Evaluation.hpp replica + Baseline CSV gate (S2.2a)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('gate',
                       help='reproduce the Baseline CSV row from rkLists.txt')
    g.add_argument('--dataset', default='aloi')
    g.add_argument('--repo', default=None,
                   help='repo root (default: inferred from this script)')
    g.add_argument('--tol-units', type=float, default=2.0,
                   help='Recall/MAP tolerance in published-precision units '
                        '(P@k is always exact at print precision)')
    e = sub.add_parser('eval', help='metrics for an arbitrary rank list')
    e.add_argument('--rk', required=True)
    e.add_argument('--bin', action='store_true',
                   help='raw u32 N*L (PHASE_RK_OUT) instead of text')
    e.add_argument('--classes', required=True)
    e.add_argument('--L', type=int, default=0,
                   help='row length of the binary (required with --bin)')
    e.add_argument('--ks', default='4,10,20,100',
                   help='comma list evaluated as BOTH P@k and Recall@k')
    e.add_argument('--map-ks', default='',
                   help='comma list evaluated as MAP@k')
    args = ap.parse_args()
    if args.cmd == 'gate':
        sys.exit(gate(args))
    sys.exit(ev(args))


if __name__ == '__main__':
    sys.exit(main())