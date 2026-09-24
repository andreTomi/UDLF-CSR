#!/usr/bin/env python3
# _p2_agg.py <mode> -- medians per arm (pre vs new) for the same-session A/B.
import os
import statistics as st
import sys

D = "/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Data/phase_profile/etapa_b/s23p2"
mode = sys.argv[1]
rad = "%s/raw_%s" % (D, mode)
tags = [f[:-4] for f in sorted(os.listdir(rad)) if f.endswith(".txt") and "warm" not in f]
arms = {"pre": [t for t in tags if t.endswith("_pre")],
        "new": [t for t in tags if t.endswith("_new")]}
print("arms: %s" % arms)


def read(tag):
    """-> (DRVAGG dict, REP totals list, DONE line, checksum set)"""
    drv, reps, done, cks = {}, [], "", set()
    for line in open("%s/raw_%s/%s.txt" % (D, mode, tag), encoding="utf-8", errors="replace"):
        p = line.strip().split(",")
        if p[0] == "DRVAGG":
            drv[p[1]] = float(p[2].split("=")[1])
        elif p[0] == "REP":
            reps.append(float(p[2]))
            cks.add(p[4])
        elif p[0] == "DONE":
            done = line.strip()
    return drv, reps, done, cks


print("=== %s: medians per run (REP cols: all / rep>0 discarded), DONE hwm ===" % mode)
data = {}
for arm, tags_ in arms.items():
    for t in tags_:
        data[t] = read(t)
        drv, reps, done = data[t][0], data[t][1], data[t][2]
        usable = reps[1:] if len(reps) > 1 else reps
        print("%-8s REP med=%8.3f (discard0=%8.3f)  hwm=%s" %
              (t, st.median(reps), st.median(usable), done.split(",")[2]))

print()
print("%-26s %>10s"%("label", "pre") if False else "%-26s %10s %10s %8s" %
      ("label", "pre(ms)", "new(ms)", "new/pre"))
keys = list(data["r1_new"][0].keys())
for k in keys:
    pre = st.median([data[t][0][k] for t in arms["pre"] if k in data[t][0]])
    new = st.median([data[t][0][k] for t in arms["new"] if k in data[t][0]])
    print("%-26s %10.3f %10.3f %7.3f" % (k, pre, new, new / pre))

pre_reps = [v for t in arms["pre"] for v in data[t][1][1:]]
new_reps = [v for t in arms["new"] for v in data[t][1][1:]]
print()
print("REP total (rep0 discarded, 6 values/arm): pre med=%.1f  new med=%.1f  ratio=%.3f"
      % (st.median(pre_reps), st.median(new_reps), st.median(new_reps) / st.median(pre_reps)))
ck_pre = {c for t in arms["pre"] for c in data[t][3]}
ck_new = {c for t in arms["new"] for c in data[t][3]}
print("checksums pre: %s" % sorted(ck_pre))
print("checksums new: %s" % sorted(ck_new))
print("GATE checksum pre==new (1 distinct value): %s" %
      ("PASS" if ck_pre == ck_new and len(ck_pre) == 1 else
       "FAIL pre=%s new=%s" % (sorted(ck_pre), sorted(ck_new))))
print("hwm set pre: %s" % sorted({data[t][2].split(",")[2] for t in arms["pre"]}))
print("hwm set new: %s" % sorted({data[t][2].split(",")[2] for t in arms["new"]}))
