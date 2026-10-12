#!/usr/bin/env python3
"""rfe_quality.py -- Fase 0 quality harness for the fast-mode RFE project.

Reuses pk_eval.py's numeric model (Evaluation.hpp replica) for the aggregate
metrics, and ADDS what the acceptance criteria require and pk_eval lacks:

  * per-query AP (same float32 accumulation as MAPStrategy, row by row)
  * paired bootstrap 95% CI of (AP_fast - AP_legacy) over queries
  * gain-relative deviation: |fast - legacy| <= 10% of (legacy - input)
  * top-n overlap fast vs legacy (diagnostic, non-blocking)
  * Recall@n (reported, non-blocking)

Labels (classes) are read HERE, in evaluation only -- never by the algorithm.

Numeric model
-------------
P@k / Recall@k / MAP@k aggregates: identical to pk_eval.compute_metrics (which
mirrors Evaluation.hpp:29-110 line by line and is gated against the published
legacy CSV). Per-query AP uses MAPStrategy's per-row value, so the mean over
queries reproduces MAP@k (self-checked in `metrics`).

Usage
-----
  python3 Source/bench/rfe_quality.py metrics --rk FILE [--bin] --L L \\
      --classes CLS --ks 5,10,20,50 --map-ks 5,10,20,50,400
  python3 Source/bench/rfe_quality.py compare --fast F --legacy G --input I \\
      --classes CLS [--bin] --L L --ks 5,10,20,50 --map-ks 5,10,20,50,400 \\
      [--primary-ap 400] [--B 2000] [--seed 42]
Exit: 0 pass, 1 blocking criterion failed, 2 usage/data error.
"""
import argparse
import sys

import numpy as np

from pk_eval import compute_metrics, die, load_classes, load_rk_bin, load_rk_text


def parse_ints(s):
    return sorted({int(x) for x in (s or '').split(',') if x.strip()})


def kmax_of(args):
    return max(parse_ints(args.ks) + parse_ints(args.map_ks) +
               [args.primary_ap or 1])


def load_named(path, args, N):
    """Load one rank file; --bin/--L apply to every file in the command."""
    if args.bin:
        if args.L <= 0:
            die('--bin requires --L')
        return load_rk_bin(path, N, args.L)
    return load_rk_text(path, N, kmax_of(args))


def per_query_ap(rk, image_ids, csize, k):
    """AP per query at cutoff k -- MAPStrategy's per-row value (f32)."""
    N = len(image_ids)
    if rk.shape[1] < k:
        die('rank lists have L=%d < requested AP cutoff k=%d' % (rk.shape[1], k))
    sub = rk[:, :k]
    sk = image_ids[sub] == image_ids[:, None]
    f32 = np.float32
    cum = sk.cumsum(axis=1, dtype=np.uint32)
    den = np.arange(1, k + 1, dtype=f32)
    terms = np.where(sk, cum.astype(f32) / den, f32(0))
    row_sum = terms.cumsum(axis=1, dtype=f32)[:, -1]
    denom = np.minimum(np.uint32(k), csize[image_ids]).astype(f32)
    return (row_sum / denom).astype(f32)


def topn_overlap(fast, legacy, n):
    """Mean over queries of |top-n fast INTERSECT top-n legacy| / n."""
    a = fast[:, :n]
    b = legacy[:, :n]
    N = a.shape[0]
    hits = 0
    for q in range(N):
        hits += len(set(a[q].tolist()).intersection(b[q].tolist()))
    return hits / (N * n)


def bootstrap_ci_mean(diff, B, seed):
    """95% percentile CI of the mean of paired per-query differences."""
    rng = np.random.default_rng(seed)
    N = diff.shape[0]
    means = np.empty(B, dtype=np.float64)
    chunk = max(1, min(B, int(2e7 // max(N, 1))))
    done = 0
    while done < B:
        m = min(chunk, B - done)
        idx = rng.integers(0, N, size=(m, N))
        means[done:done + m] = diff[idx].mean(axis=1)
        done += m
    lo, hi = np.percentile(means, [2.5, 97.5])
    return float(lo), float(hi), float(diff.mean())


def cmd_metrics(args):
    image_ids, csize = load_classes(args.classes)
    N = len(image_ids)
    rk = load_named(args.rk, args, N)
    ks_p = parse_ints(args.ks)
    ks_m = parse_ints(args.map_ks)
    agg = compute_metrics(rk, image_ids, csize, ks_p, ks_p, ks_m)
    for name in sorted(agg, key=lambda s: (s.split('@')[0], int(s.split('@')[1]))):
        print('METRIC,%s,%.6f' % (name, agg[name]))
    # self-check: mean per-query AP == aggregate MAP@k (same f32 model)
    rc = 0
    for k in ks_m:
        ap = per_query_ap(rk, image_ids, csize, k)
        acc = np.float32(0)
        for v in ap:
            acc = np.float32(acc + v)
        mean_map = float(acc / np.float32(N)) * 100.0
        ok = abs(mean_map - agg['MAP@%d' % k]) <= 2e-4
        print('SELFCHK,MAP@%d,perquery=%.6f,agg=%.6f,%s'
              % (k, mean_map, agg['MAP@%d' % k], 'PASS' if ok else 'FAIL'))
        rc = rc or (0 if ok else 1)
    return rc


def cmd_compare(args):
    image_ids, csize = load_classes(args.classes)
    N = len(image_ids)
    ks_p = parse_ints(args.ks)
    ks_m = parse_ints(args.map_ks)
    rk_f = load_named(args.fast, args, N)
    rk_l = load_named(args.legacy, args, N)
    rk_i = load_named(args.input, args, N)

    m_f = compute_metrics(rk_f, image_ids, csize, ks_p, ks_p, ks_m)
    m_l = compute_metrics(rk_l, image_ids, csize, ks_p, ks_p, ks_m)
    m_i = compute_metrics(rk_i, image_ids, csize, ks_p, ks_p, ks_m)

    blocking = []
    print('QUAL,dataset_n,%d' % N)

    # ---- C1: relative metric equivalence (MAP and every P@n) ----------------
    rel_targets = [('MAP@%d' % k, args.rel_map) for k in ks_m] + \
                  [('P@%d' % k, args.rel_p) for k in ks_p]
    for name, tol_pct in rel_targets:
        fast, leg = m_f[name], m_l[name]
        rel = (abs(fast - leg) / leg * 100.0 if leg != 0
               else (0.0 if fast == 0 else float('inf')))
        ok = rel <= tol_pct
        print('QUAL,rel_%s,fast=%.6f,legacy=%.6f,delta_rel_pct=%.4f,'
              'tol_pct=%.2f,%s' % (name, fast, leg, rel, tol_pct,
                                   'PASS' if ok else 'FAIL'))
        blocking.append(('C1:' + name, ok))

    # ---- C2: deviation <= 10% of the RFE gain over the input ranking -------
    for name in ['MAP@%d' % k for k in ks_m] + ['P@%d' % k for k in ks_p]:
        gain = m_l[name] - m_i[name]
        dev = abs(m_f[name] - m_l[name])
        if gain == 0:
            ok = dev == 0.0
            print('QUAL,gain_%s,input=%.6f,legacy=%.6f,gain=0,dev=%.6f,%s'
                  % (name, m_i[name], m_l[name], dev, 'PASS' if ok else 'FAIL'))
        else:
            bound = abs(gain) * (args.gain_frac / 100.0)
            ok = dev <= bound + 1e-12
            print('QUAL,gain_%s,input=%.6f,legacy=%.6f,gain=%.6f,dev=%.6f,'
                  'bound=%.6f (%.0f%% of gain),%s'
                  % (name, m_i[name], m_l[name], gain, dev, bound,
                     args.gain_frac, 'PASS' if ok else 'FAIL'))
        blocking.append(('C2:' + name, ok))

    # ---- C3: paired bootstrap CI of (AP_fast - AP_legacy) ------------------
    primary = args.primary_ap or max(ks_m)
    ap_f = per_query_ap(rk_f, image_ids, csize, primary)
    ap_l = per_query_ap(rk_l, image_ids, csize, primary)
    diff = ap_f.astype(np.float64) - ap_l.astype(np.float64)
    lo, hi, mean_d = bootstrap_ci_mean(diff, args.B, args.seed)
    ok = (lo <= 0.0 <= hi) or (lo >= 0.0)  # contains 0 OR favors the fast arm
    print('QUAL,bootstrap_ap@%d,B=%d,seed=%d,mean_diff=%.6g,ci95=[%.6g,%.6g],%s'
          % (primary, args.B, args.seed, mean_d, lo, hi,
             'PASS' if ok else 'FAIL'))
    blocking.append(('C3:bootstrap', ok))
    # diagnostics at the other reported cutoffs (non-blocking)
    for k in ks_m:
        if k == primary:
            continue
        d = (per_query_ap(rk_f, image_ids, csize, k).astype(np.float64) -
             per_query_ap(rk_l, image_ids, csize, k).astype(np.float64))
        lo2, hi2, m2 = bootstrap_ci_mean(d, args.B, args.seed)
        print('QUAL,bootstrap_diag_ap@%d,mean_diff=%.6g,ci95=[%.6g,%.6g],INFO'
              % (k, m2, lo2, hi2))

    # ---- Recall@n: reported, never blocking --------------------------------
    for k in ks_p:
        print('QUAL,recall@%d,fast=%.6f,legacy=%.6f,input=%.6f,INFO'
              % (k, m_f['Recall@%d' % k], m_l['Recall@%d' % k],
                 m_i['Recall@%d' % k]))

    # ---- top-n overlap: diagnostic, never blocking -------------------------
    for k in ks_p:
        print('QUAL,overlap@%d,fast_vs_legacy=%.6f,INFO'
              % (k, topn_overlap(rk_f, rk_l, k)))

    failed = [name for name, ok in blocking if not ok]
    if failed:
        print('QUALVERDICT,FAIL,criteria_failed=%d,%s'
              % (len(failed), ';'.join(failed)))
        return 1
    print('QUALVERDICT,PASS,criteria=%d' % len(blocking))
    return 0


def add_load_args(p):
    p.add_argument('--classes', required=True)
    p.add_argument('--bin', action='store_true',
                   help='raw u32 N*L files instead of text')
    p.add_argument('--L', type=int, default=0,
                   help='row length of the u32 binary (required with --bin)')
    p.add_argument('--ks', default='5,10,20,50',
                   help='comma list for P@k and Recall@k')
    p.add_argument('--map-ks', default='5,10,20,50',
                   help='comma list for MAP@k')
    p.add_argument('--primary-ap', type=int, default=None,
                   help='cutoff for the paired bootstrap AP (default: max map-ks)')


def main():
    ap = argparse.ArgumentParser(
        description='Fase 0 quality harness (MAP/P@n/Recall + acceptance gate)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('metrics', help='aggregate metrics of one rank list')
    m.add_argument('--rk', required=True)
    add_load_args(m)
    c = sub.add_parser('compare', help='fast vs legacy vs input acceptance gate')
    c.add_argument('--fast', required=True)
    c.add_argument('--legacy', required=True)
    c.add_argument('--input', required=True)
    add_load_args(c)
    c.add_argument('--B', type=int, default=2000, help='bootstrap resamples')
    c.add_argument('--seed', type=int, default=42)
    c.add_argument('--rel-map', type=float, default=1.0,
                   help='max relative MAP deviation in %% (default 1.0)')
    c.add_argument('--rel-p', type=float, default=1.0,
                   help='max relative P@n deviation in %% (default 1.0)')
    c.add_argument('--gain-frac', type=float, default=10.0,
                   help='max deviation as %% of the RFE gain (default 10.0)')
    args = ap.parse_args()
    if args.cmd == 'metrics':
        sys.exit(cmd_metrics(args))
    sys.exit(cmd_compare(args))


if __name__ == '__main__':
    main()

