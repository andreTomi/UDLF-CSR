#!/usr/bin/env python3
"""agg_table.py -- consolidate the Fase 0 raw logs into one table.

Scans Data/phase_profile/rfe_fast/raw/run_*.log, extracts the RFEQ timing/RSS/ck
line and the METRIC, lines each driver already wrote, and emits table.csv:

    dataset,L,k,cc,arm,wall_s,rss_kb,ck_rk,MAP@L,P@10,P@20,Recall@10

MAP@L uses the L-width cutoff (what the legacy effectively computes); P@n and
Recall@n are at n=10,20 (representative of the {5,10,20,50} family already in
each log). One row per (dataset,L,k,cc,arm). Missing metrics -> empty cell.
"""
import csv
import os
import re
import sys

RAW = sys.argv[1] if len(sys.argv) > 1 else \
    'Data/phase_profile/rfe_fast/raw'

rfeq_re = re.compile(r'RFEQ wall_s=([0-9.]+) VmHWM_kb=(\d+) ck_rk=(0x[0-9a-f]+)')
arm_re = re.compile(r'RFEQ arm=(\w+) N=(\d+) L=(\d+) k=(\d+) .*cc=(\d)')
metric_re = re.compile(r'METRIC,(\S+?),([0-9.]+)')


def parse_log(path):
    arm = wall = rss = ck = None
    L = k = cc = None
    metrics = {}
    with open(path, 'r', errors='replace') as f:
        for ln in f:
            m = arm_re.search(ln)
            if m:
                L, k, cc = m.group(3), m.group(4), m.group(5)
            m = rfeq_re.search(ln)
            if m:
                wall, rss, ck = m.group(1), m.group(2), m.group(3)
            m = metric_re.search(ln)
            if m:
                metrics[m.group(1)] = m.group(2)
    return dict(L=L, k=k, cc=cc, wall=wall, rss=rss, ck=ck, metrics=metrics)


rows = []
for fn in sorted(os.listdir(RAW)):
    mo = re.match(r'run_(.+?)_(input|legacy|exact|fast)\.log$', fn)
    if not mo:
        continue
    tag, arm = mo.group(1), mo.group(2)
    parts = tag.split('_')            # ds_L.._k..[_cc]
    ds = parts[0]
    L = next((p[1:] for p in parts if p.startswith('L')), '')
    k = next((p[1:] for p in parts if p.startswith('k')), '')
    cc = '1' if tag.endswith('_cc') else '0'
    d = parse_log(os.path.join(RAW, fn))
    if d['L']:
        L = d['L']
    if d['k']:
        k = d['k']
    if d['cc'] is not None:
        cc = d['cc']
    met = d['metrics']
    mapl = met.get('MAP@%s' % L, '')
    rows.append([ds, L, k, cc, arm, d['wall'] or '', d['rss'] or '',
                 d['ck'] or '', mapl,
                 met.get('P@10', ''), met.get('P@20', ''),
                 met.get('Recall@10', '')])

hdr = ['dataset', 'L', 'k', 'cc', 'arm', 'wall_s', 'rss_kb', 'ck_rk',
       'MAP@L', 'P@10', 'P@20', 'Recall@10']
out = os.path.join(RAW, 'table.csv')
with open(out, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(hdr)
    w.writerows(rows)
print('wrote %s (%d data rows)' % (out, len(rows)))
for r in rows:
    print(','.join(str(x) for x in r))
