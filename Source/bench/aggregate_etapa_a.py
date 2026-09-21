#!/usr/bin/env python3
# aggregate_etapa_a.py -- aggregation for the Etapa A campaign (A1/A2/A3/A5).
#
# Input : raw stdout captures from bench_phase (run_etapa_a.sh, one file per
#         configuration).
# Output: CSV sections on stdout:
#   CURVE    total_ms per (workload, cpuset, threads): median/min/max
#   PHASES   median per engine phase (total/wall/skew/user/sys/faults/ctx)
#   LEDGER   driver statements + engine call-level walls vs REP
#   MEMBUILD per-build memory ledger (delta HWM per emitted triplet)
#   CTX      measured bound / ratio / branch (phase-4 calibration input)
#
# Protocol: rep 0 discarded, median over the rest; min/max reported so noise is
# visible instead of hidden.
import statistics
import sys
from collections import defaultdict

CALL_PHASES = ["MP_CALL", "MERGE_CALL", "COMBINE_CALL", "BUILD_CALL",
               "TRANSPOSE", "ALLOC_SETUP", "ALLOC_TEARDOWN"]


def rx(pattern, text, default=""):
    import re
    m = re.search(pattern, text)
    return m.group(1) if m else default


def parse(path):
    meta = {"file": path}
    reps = {}
    drv = defaultdict(list)
    membuild = []
    mem_points = []
    ctx = None
    cur = None
    try:
        fh = open(path, errors="replace")
    except OSError as e:
        return meta, reps, drv, membuild, mem_points, ctx, str(e)
    with fh:
        for line in fh:
            line = line.strip()
            if line.startswith("RUN,"):
                p = line.split(",")
                if len(p) >= 6:
                    meta.update(mode=p[1], workload=p[2], threads=p[3],
                                reps=p[4], flag=p[5])
            elif line.startswith("TSET,"):
                # affinity_omp=0,1,2,3 contains commas -> regex, not a CSV split.
                meta["omp_threads"] = rx(r"omp_threads=(\d+)", line, "")
                meta["affinity_omp"] = rx(r"affinity_omp=([\d,]*)", line, "")
                meta["affinity_at_entry"] = rx(r"affinity_at_entry=([\d,]*)", line, "")
                meta["req_mask"] = rx(r"req_mask=([^,]*)", line, "")
                meta["tbb_max"] = rx(r"tbb_max=(\d+)", line, "")
                meta["cpus"] = rx(r"cpus=(\d+)", line, "")
                meta["max_concurrency"] = rx(r"max_concurrency=(\d+)", line, "")
            elif line.startswith("REAL,") or line.startswith("PARAMS,"):
                meta["params"] = line
            elif line.startswith("EXTRACT,"):
                meta["extract"] = line
            elif line.startswith("REP,"):
                p = line.split(",")
                cur = int(p[1])
                reps[cur] = {"rep": cur, "total_ms": float(p[2]),
                             "nnz": int(p[3]), "checksum": p[4], "phases": {}}
            elif line.startswith("DRVAGG,"):
                p = line.split(",")
                kv = dict(x.split("=") for x in p[2:] if "=" in x)
                drv[p[1]].append(float(kv["median_ms"]))
            elif line.startswith("PHASEPROFILE,MEMBUILD,"):
                kv = dict(x.split("=") for x in line.split(",")[2:] if "=" in x)
                membuild.append({k: float(v) for k, v in kv.items()})
            elif line.startswith("PHASEPROFILE,MEM,"):
                mem_points.append(line.split(",")[2])
            elif line.startswith("PHASEPROFILE,CTX,"):
                ctx = dict(x.split("=") for x in line.split(",")[2:] if "=" in x)
                ctx["_raw"] = line          # bound_sum/rows are re-derived
            elif line.startswith("PHASEPROFILE,HWM,"):
                meta["hwm"] = dict(x.split("=") for x in line.split(",")[2:]
                                   if "=" in x)
            elif line.startswith("PHASEPROFILE,"):
                p = line.split(",")
                d = {}
                for kv in p[2:]:
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        try:
                            d[k] = float(v)
                        except ValueError:
                            pass
                if cur is not None:
                    reps[cur]["phases"][p[1]] = d
    return meta, reps, drv, membuild, mem_points, ctx, None


def med(vals):
    return statistics.median(vals) if vals else 0.0


def tag_cpuset(tag):
    """label -> (workload, cpuset_csvsafe, family).

    The label shape is <family>_<workload>_c<cpuset>_t<N>, e.g.
      curve_w1_c0-1_t2        -> (w1,  0-1,    curve)
      curve_sib_w3_c0+2_t3    -> (w3,  0+2,    sib)
      pipe_w1_call_t12        -> (w1,  all,    pipe)
      mem_w3_c0_t1            -> (w3,  0,      mem)
      real_lhrr_aloi_c0-5_t6  -> (lhrr_aloi, 0-5, real)
    NOTE for real_*: the workload name lives in the *prefix*, not in the
    remainder, so the prefix must not be stripped before splitting.
    """
    family = "other"
    for k in ("real_lhrr_", "real_cprr_", "curve_sib_", "curve_",
              "pipe_", "phases_", "mem_"):
        if tag.startswith(k):
            family = k.rstrip("_")
            tag = tag[len(k):]
            break
    if family.startswith("real_"):
        # after stripping "real_lhrr_" the remainder is  <dataset>_<cpuset>_t<N>
        pipeline = family.split("_", 1)[1]
        head = tag.rsplit("_t", 1)[0]              # aloi_c0-5
        dataset, _, cpus = head.rpartition("_")    # aloi , c0-5
        return pipeline + "_" + dataset, cpus.replace(",", "+"), "real"
    cpus = tag.rsplit("_t", 1)[0] if "_t" in tag else tag
    wl = tag[:len(tag) - len(cpus) - 1] if len(tag) > len(cpus) + 1 else tag
    if family == "sib":
        family = "curve"
    return wl, cpus.replace(",", "+"), family


def rep_medians(reps):
    """Rep 0 discarded (warm-up); totals and per-phase medians over the rest."""
    use = [r for r in reps.values() if r["rep"] > 0]
    if not use:
        use = list(reps.values())
    totals = [r["total_ms"] for r in use]
    nnz = {r["nnz"] for r in use}
    cks = {r["checksum"] for r in use}
    per_phase = defaultdict(lambda: defaultdict(list))
    for r in use:
        for ph, d in r["phases"].items():
            for k, v in d.items():
                per_phase[ph][k].append(v)
    return totals, nnz, cks, per_phase


def emit_rows(header, rows):
    print(",".join(header))
    for r in rows:
        print(",".join(str(x) for x in r))


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: aggregate_etapa_a.py <raw_dir>")
    import glob
    import os
    files = sorted(glob.glob(os.path.join(sys.argv[1], "*.txt")))

    curve = []          # no-flag totals
    phases = []         # flag phase tables
    ledger = []         # pipe statements
    membuild = []       # per-build memory ledger
    ctx = []            # CTX counters (bound/ratio/branch)
    masks = []          # affinity audit (pre-bind mask vs label)

    for path in files:
        tag = os.path.basename(path)[:-4]
        meta, reps, drv, mbs, mem_points, ctxd, err = parse(path)
        if err:
            print(f"WARN unreadable {path}: {err}", file=sys.stderr)
            continue
        wl, cpus, family = tag_cpuset(tag)
        th = meta.get("omp_threads", "?")
        masks.append([tag, family, wl, cpus, th,
                      meta.get("req_mask", "(absent)"),
                      meta.get("affinity_omp", ""), meta.get("cpus", "")])
        totals, nnzs, cks, per_phase = rep_medians(reps)
        if not totals:
            continue
        med_total = med(totals)
        r = [wl, cpus, th, len(totals), f"{med_total:.1f}",
             f"{min(totals):.1f}", f"{max(totals):.1f}",
             ";".join(str(x) for x in sorted(nnzs)),
             ";".join(sorted(cks))]
        if meta.get("hwm"):
            r.append(meta["hwm"].get("final_kb", ""))
        else:
            r.append("")
        r.append(family)
        curve.append(r)
        # phase table rows (flag builds)
        for ph, d in per_phase.items():
            phases.append([wl, cpus, th, ph,
                           f"{med(d.get('total_ms', [0])):.2f}",
                           f"{med(d.get('wall_ms', [0])):.2f}",
                           f"{med(d.get('skew_ms', [0])):.2f}",
                           f"{med(d.get('user_ms', [0])):.2f}",
                           f"{med(d.get('sys_ms', [0])):.2f}",
                           f"{med(d.get('minflt', [0])):.0f}",
                           f"{med(d.get('calls', [0])):.0f}"])
        # ledger: pipe runs -> top-level call walls vs REP total.
        # NESTING NOTE: BUILD_CALL / ALLOC_SETUP / ALLOC_TEARDOWN are counted
        # *inside* MP_CALL / MERGE_CALL / COMBINE_CALL (multiply_and_prune
        # calls build_from_partial_results and manages its allocators
        # internally), so they are excluded from the top-level sum and kept
        # for the per-call breakdown instead.
        if tag.startswith("pipe"):
            top_phases = ["MP_CALL", "MERGE_CALL", "COMBINE_CALL", "TRANSPOSE"]
            call_total = 0.0
            for name in top_phases:
                vals = per_phase.get(name, {}).get("total_ms", [])
                if vals:
                    call_total += med(vals)
            inner = 0.0
            for name in ("BUILD_CALL", "ALLOC_SETUP", "ALLOC_TEARDOWN"):
                vals = per_phase.get(name, {}).get("total_ms", [])
                if vals:
                    inner += med(vals)
            ledger.append([wl, cpus, th,
                           f"{med_total:.1f}",
                           f"{call_total:.1f}",
                           f"{med_total - call_total:.1f}",
                           f"{100.0 * (med_total - call_total) / med_total:.2f}",
                           f"{inner:.1f}"])
        for row in mbs:
            membuild.append([tag, wl, cpus, th] +
                            [row.get(k, "") for k in
                             ("n", "emitted", "tmp_bytes", "out_bytes",
                              "alloc_bytes", "count_bytes", "delta_hwm_kb",
                              "bytes_per_triplet", "reset_rc")])
        if ctxd:
            # CTX carries bound_sum (accumulated per row) and max_bound, so the
            # mean bound is derived here; ratio/branch are emitted directly.
            bs = float(rx(r"bound_sum=(\d+)", ctxd.get("_raw", ""), "0") or 0)
            nrows = float(rx(r"rows=(\d+)", ctxd.get("_raw", ""), "0") or 0)
            ctx.append([wl, cpus, th,
                        f"{bs / nrows:.1f}" if nrows else "",
                        ctxd.get("max_bound", ""),
                        ctxd.get("ratio", ""),
                        ctxd.get("branch", ""),
                        ctxd.get("emitted", ""),
                        ctxd.get("rows", "")])

    print("== CURVE (median over reps, rep0 discarded) ==")
    emit_rows(["workload", "cpus", "threads", "n", "med_ms", "min_ms",
               "max_ms", "nnz", "checksum", "hwm_kb", "family"], curve)
    print("== PHASES (median per phase) ==")
    emit_rows(["workload", "cpus", "threads", "phase", "total_ms", "wall_ms",
               "skew_ms", "user_ms", "sys_ms", "minflt", "calls"], phases)
    print("== LEDGER (pipe: REP total vs top-level call walls; nesting excluded) ==")
    emit_rows(["workload", "cpus", "threads", "rep_ms", "call_ms",
               "residual_ms", "residual_pct", "inner_build_alloc_ms"], ledger)
    print("== MEMBUILD ==")
    emit_rows(["file", "workload", "cpus", "threads", "n", "emitted",
               "tmp_bytes", "out_bytes", "alloc_bytes", "count_bytes",
               "delta_hwm_kb", "bytes_per_triplet", "reset_rc"], membuild)
    print("== CTX (measured bound / ratio / branch: phase-4 calibration input) ==")
    emit_rows(["workload", "cpus", "threads", "bound_mean", "bound_max",
               "ratio", "branch", "emitted", "rows"], ctx)
    print("== MASK (req_mask from PHASE_MASK; affinity_omp is the OMP-bound master) ==")
    emit_rows(["file", "family", "workload", "cpus_label", "threads",
               "req_mask", "affinity_omp", "cpus_online"], masks)
    # consistency: one nnz/checksum per (family, workload) -- a workload must
    # give the same result across cpusets/threads, and families must not be
    # mixed (mem_* does fewer rebuilds, so it is compared inside itself only).
    print("== CHECK (distinct nnz|checksum per (family,workload); must be 1) ==")
    per_key = defaultdict(set)
    for r in curve:
        per_key[(r[1], r[0])].add((r[7], r[8]))
    for key in sorted(per_key):
        print(f"{key[0]},{key[1]},variants={len(per_key[key])}")


if __name__ == "__main__":
    main()
