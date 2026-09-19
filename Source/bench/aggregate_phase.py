#!/usr/bin/env python3
# aggregate_phase.py -- Phase 0 aggregation for bench_phase output.
#
# Input : one or more bench_phase stdout captures.
# Output: CSV -- one row per phase with the median across reps (rep 0 is
#         discarded, per the measurement protocol), plus per-run medians.
#
# Notes:
#   * F1_ACCUM / F2_PRUNE ticks are collected only on timed rows. When
#     PHASE_PROFILE_ROW_SAMPLE > 1 the script extrapolates:
#     extrap_ms = total_ms * (rows_total / rows_f1)  (ctx2 / ctx1).
#   * total_ms is the SUM over threads (thread-seconds); wall_ms is the
#     region makespan; skew_ms = max(end) - mean(end) over threads.
#
# Usage: python3 aggregate_phase.py out_w1_t12.txt [...] > table.csv
import statistics
import sys

def parse(path):
    reps = {}          # rep index -> dict(REP + phases)
    meta = {"file": path}
    cur = None
    for line in open(path, errors="replace"):
        line = line.strip()
        if line.startswith("RUN,"):
            p = line.split(",")
            meta.update(mode=p[1], workload=p[2], threads=p[3], reps=p[4], flag=p[5])
        elif line.startswith("REP,"):
            p = line.split(",")
            cur = int(p[1])
            reps[cur] = {"rep": cur, "total_ms": float(p[2]),
                         "nnz": int(p[3]), "checksum": p[4], "phases": {}}
        elif line.startswith("PHASEPROFILE,HWM,"):
            if cur is None:
                continue
            kv = dict(x.split("=") for x in line.split(",")[2:])
            reps[cur]["hwm"] = {k: int(v) for k, v in kv.items()}
        elif line.startswith("PHASEPROFILE,"):
            if cur is None:
                continue
            parts = line.split(",")
            name = parts[1]
            d = {}
            for kv in parts[2:]:
                k, v = kv.split("=")
                d[k] = float(v)
            reps[cur]["phases"][name] = d
        elif line.startswith("DONE,"):
            meta["done"] = line
    return meta, reps

def main():
    if len(sys.argv) < 2:
        print("usage: aggregate_phase.py out1.txt [out2.txt ...]", file=sys.stderr)
        return 2
    runs = [parse(p) for p in sys.argv[1:]]

    m0 = runs[0][0]
    print("AGG,mode=%s,workload=%s,threads=%s,flag=%s,nfiles=%d" %
          (m0["mode"], m0["workload"], m0["threads"], m0["flag"], len(runs)))

    # Rep-level medians (discard rep 0).
    tot = [r["total_ms"] for _, reps in runs for i, r in reps.items() if i > 0]
    print("REP_MEDIAN,total_ms=%.3f" % statistics.median(tot))

    nnzs = {r["nnz"] for _, reps in runs for r in reps.values()}
    cks  = {r["checksum"] for _, reps in runs for r in reps.values()}
    print("CONSISTENCY,nnz_unique=%d,checksum_unique=%d" % (len(nnzs), len(cks)))

    # Per-phase medians across all reps (rep 0 discarded).
    phases = sorted({p for _, reps in runs for r in reps.values()
                     for p in r["phases"]})
    hdr = ("phase", "total_ms", "wall_ms", "skew_ms", "extrap_ms",
           "user_ms", "sys_ms", "minflt", "majflt", "ctx1", "ctx2")
    print("PHASES," + ",".join(hdr))
    for ph in phases:
        vals = {k: [] for k in hdr[1:]}
        for _, reps in runs:
            for i, r in reps.items():
                if i == 0 or ph not in r["phases"]:
                    continue
                d = r["phases"][ph]
                vals["total_ms"].append(d["total_ms"])
                vals["wall_ms"].append(d["wall_ms"])
                vals["skew_ms"].append(d["skew_ms"])
                vals["user_ms"].append(d["user_ms"])
                vals["sys_ms"].append(d["sys_ms"])
                vals["minflt"].append(d["minflt"])
                vals["majflt"].append(d["majflt"])
                vals["ctx1"].append(d["ctx1"])
                vals["ctx2"].append(d["ctx2"])
                if d["ctx1"] > 0 and d["ctx2"] >= d["ctx1"]:
                    vals["extrap_ms"].append(
                        d["total_ms"] * (d["ctx2"] / d["ctx1"]))
                else:
                    vals["extrap_ms"].append(d["total_ms"])
        med = [statistics.median(vals[k]) if vals[k] else 0.0 for k in hdr[1:]]
        print("PHASE,%s,%s" % (ph, ",".join("%.3f" % v for v in med)))

    # Peak memory: max across reps of build-entry/exit VmHWM; final from last rep.
    hb = [r["hwm"]["before_build_max_kb"] for _, reps in runs
          for i, r in reps.items() if i > 0 and "hwm" in r]
    ha = [r["hwm"]["after_build_max_kb"] for _, reps in runs
          for i, r in reps.items() if i > 0 and "hwm" in r]
    last = max((max(reps) for _, reps in runs), default=None)
    fin = runs[0][1].get(last, {}).get("hwm", {}).get("final_kb", -1)
    print("HWM_MEDMAX,before_build_kb=%d,after_build_kb=%d,final_kb=%d" %
          (int(statistics.median(hb)) if hb else -1,
           int(statistics.median(ha)) if ha else -1, int(fin)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
