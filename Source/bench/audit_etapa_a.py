#!/usr/bin/env python3
# audit_etapa_a.py -- integrity audit for the Etapa A raw captures.
#
# Why: the curve labels claim a cpuset ("c0-5", "c0,2,4"), but the label is
# produced by the shell while the *effective* affinity is reported by the
# driver on the TSET line (sched_getaffinity). A leaked taskset from the
# invoking shell or a mislabeled file silently corrupts the scaling curve.
# This script cross-checks one against the other and flags mismatches.
#
# Usage: python3 Source/bench/audit_etapa_a.py [rawdir]
import os
import re
import sys

ALL_CPUS = set(range(12))


def expand(mask):
    """'0-3' / '0,2,4' / 'all' -> set of cpu ids (None if unparsable)."""
    if mask in ("all", "call", None):
        return set(ALL_CPUS)
    out = set()
    for part in mask.split("+"):
        part = part.replace(",", "+")
    for part in re.split(r"[+,]", mask):
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        else:
            out.add(int(part))
    return out


def claimed_mask(tag):
    """Mask the filename claims, or None when the tag carries no cpuset."""
    m = re.search(r"_c(0[0-9,+\-]*)_t\d+$", tag)
    if m:
        return m.group(1), "cpuset"
    m = re.search(r"_call_t\d+$", tag)
    if m:
        return "all", "cpuset"
    m = re.search(r"_c(\d+)_t\d+$", tag)
    if m:
        return m.group(1), "cpuset"
    return None, "none"


def main():
    raw = sys.argv[1] if len(sys.argv) > 1 else "Data/phase_profile/etapa_a/raw"
    rows = []
    for name in sorted(os.listdir(raw)):
        if not name.endswith(".txt"):
            continue
        tag = name[:-4]
        aff = None
        threads = cc = None
        wl = mode = None
        nrep = 0
        with open(os.path.join(raw, name), errors="replace") as fh:
            for line in fh:
                if line.startswith("TSET,"):
                    # NOTE: affinity=0,1,2,3 holds commas, so the field cannot
                    # be split on "," -- it is bounded by ",cpus=" instead.
                    line = line.strip()
                    m = re.search(r"omp_threads=(\d+)", line)
                    threads = m.group(1) if m else None
                    m = re.search(r"affinity=([\d,]*),cpus=", line)
                    aff = m.group(1) if m else None
                    m = re.search(r"max_concurrency=(\d+)", line)
                    cc = m.group(1) if m else None
                elif line.startswith("RUN,"):
                    p = line.strip().split(",")
                    mode, wl = p[1], p[2]
                elif line.startswith("REP,"):
                    nrep += 1
        if aff is None:
            rows.append((tag, "NO-TSET", "", "", "", "", 0))
            continue
        got = set(int(x) for x in aff.split(",") if x.isdigit())
        cmask, kind = claimed_mask(tag)
        if kind == "none":
            status = "no-mask-in-name"
            want = None
        else:
            want = expand(cmask)
            status = "OK" if got == want else "MISMATCH"
        rows.append((tag, status, aff, threads, cc,
                     (("%s" % sorted(want)) if want is not None else ""),
                     nrep, mode, wl))

    print("tag,status,effective_affinity,threads,tbb_max_conc,requested,reps,mode,workload")
    bad = 0
    for r in rows:
        r = list(r) + [""] * (9 - len(r))
        print(",".join(str(x) for x in r[:9]))
        if r[1] == "MISMATCH":
            bad += 1
    print(f"\n# files={len(rows)} mismatches={bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
