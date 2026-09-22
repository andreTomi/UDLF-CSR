#!/usr/bin/env python3
"""A5-bis aggregator: real-dataset (ALOI, LHRR) captures of bench_phase `real`.

Usage:
    python3 Source/bench/a5bis_agg.py <dir_bis> [<dir_ref>]

Reduction rules (same as ab_agg.py, so the numbers are comparable with the
synthetic campaign):
  * wall clock (REP) and every PHASEPROFILE phase: median over reps 1..n with
    rep 0 (warmup) discarded; blocks are delimited by the REP line that starts
    each repetition.
  * the DRVAGG ledger is what the driver itself prints once at the end
    (median over ALL reps, rep 0 included -- documented caveat, the driver does
    not keep per-rep samples for the call stages).
  * VmHWM comes from the DONE line (peak RSS of the process, kB).

With <dir_ref> the captures are matched by tag and the deltas (bis - ref) are
printed for the total, the peak RSS and every ledger stage.
"""
import glob
import os
import statistics as st
import sys

PHASES = ['F1_ACCUM', 'F2_PRUNE', 'MULT_TOTAL', 'BUILD_CALL', 'COMBINE_CALL',
          'TRANSPOSE', 'MP_CALL']


def parse(path):
    reps, blocks, block = [], [], {}
    phases, drvagg, hwm, threads, reps_decl = {}, {}, None, None, None
    for line in open(path):
        p = line.rstrip('\n').split(',')
        if p[0] == 'RUN':
            threads = int(p[3]); reps_decl = int(p[4])
        elif p[0] == 'REP':
            blocks.append(block); block = {}
            reps.append(float(p[2]))
        elif p[0] == 'PHASEPROFILE':
            name = p[1]
            if name in PHASES:
                block[name] = float(p[2].split('=')[1])
            elif name == 'HWM':
                pass
        elif p[0] == 'DRVAGG':
            drvagg[p[1]] = float(p[2].split('=')[1])   # median_ms
        elif p[0] == 'DONE':
            hwm = int(p[2])                            # vmhwm_kb
    blocks.append(block)
    blocks = blocks[1:]                                # drop the pre-REP block
    for b in blocks[1:]:                               # rep 0 = warmup
        for k, v in b.items():
            phases.setdefault(k, []).append(v)
    med = lambda v: st.median(v) if v else float('nan')
    return dict(tag=os.path.basename(path)[:-4], threads=threads, n=reps_decl,
                reps=reps, med=med(reps[1:] or reps),
                lo=min(reps[1:] or reps), hi=max(reps[1:] or reps),
                phases={k: med(v) for k, v in phases.items()},
                ledger=drvagg, hwm=hwm)


def load(d):
    out = {}
    for f in sorted(glob.glob(os.path.join(d, '*.txt'))):
        if os.path.basename(f).startswith('extract'):
            continue
        r = parse(f)
        out[r['tag']] = r
    return out


def main():
    bis = load(sys.argv[1])
    ref = load(sys.argv[2]) if len(sys.argv) > 2 else {}

    hdr = (f'{"capture":<26}{"T":>3}{"REP_med":>10}{"min":>9}{"max":>9}'
           f'{"VmHWM_MB":>10}{"F1_ACC":>9}{"F2_PRUN":>9}{"MULT":>10}'
           f'{"BUILD":>9}{"TRANSP":>8}')
    print('== A5-bis (LHRR real, ALOI N=72000 L=400 k=10 limit=400, T=1) ==')
    print(hdr)
    for tag, r in sorted(bis.items(), key=lambda kv: kv[1]['threads']):
        ph = r['phases']
        g = lambda k: ph.get(k, float('nan'))
        print(f'{tag:<26}{r["threads"]:>3}{r["med"]:>10.1f}{r["lo"]:>9.1f}'
              f'{r["hi"]:>9.1f}{(r["hwm"] or 0)/1024:>10.1f}{g("F1_ACCUM"):>9.1f}'
              f'{g("F2_PRUNE"):>9.1f}{g("MULT_TOTAL"):>10.1f}'
              f'{g("BUILD_CALL"):>9.1f}{g("TRANSPOSE"):>8.1f}')

    if not ref:
        return
    print('\n== bis vs Etapa A (mesma config, kernel pre-Fase 1) ==')
    print(f'{"capture":<26}{"T":>3}{"ref_med":>10}{"ref_min":>9}{"ref_max":>9}'
          f'{"bis_med":>10}{"bis_min":>9}{"bis_max":>9}{"speedup":>9}'
          f'{"ref_MB":>8}{"bis_MB":>8}{"dMB%":>8}')
    for tag, r in sorted(bis.items(), key=lambda kv: kv[1]['threads']):
        o = ref.get(tag)
        if not o:
            continue
        sp = o['med'] / r['med'] if r['med'] else float('nan')
        rb, ob = r['hwm'] or 0, o['hwm'] or 0
        print(f'{tag:<26}{r["threads"]:>3}{o["med"]:>10.1f}{o["lo"]:>9.1f}'
              f'{o["hi"]:>9.1f}{r["med"]:>10.1f}{r["lo"]:>9.1f}{r["hi"]:>9.1f}'
              f'{sp:>9.2f}{ob/1024:>8.1f}{rb/1024:>8.1f}'
              f'{100.0*(rb-ob)/ob:>8.1f}')

    print('\n== ledger (DRVAGG, mediana de todos os reps) ==')
    keys = ['restore_rk_state', 'position_build', 'update_ranked_lists_pos',
            'H_build', 'H_transpose', 'mult_S', 'mult_Srev',
            'combine_S_total', 'combine_main', 'update_ranked_lists',
            'iter_free']
    tags = [t for t in sorted(bis, key=lambda k: bis[k]['threads'])]
    print(f'{"stage":<26}' + ''.join(f'{"bis_t" + str(bis[t]["threads"]):>12}'
                                     for t in tags)
          + ''.join(f'{"ref_t" + str(ref[t]["threads"]):>12}'
                    for t in tags if t in ref))
    for k in keys:
        row = f'{k:<26}'
        for t in tags:
            row += f'{bis[t]["ledger"].get(k, float("nan")):>12.1f}'
        for t in tags:
            if t in ref:
                row += f'{ref[t]["ledger"].get(k, float("nan")):>12.1f}'
        print(row)


if __name__ == '__main__':
    main()
