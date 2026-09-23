#!/usr/bin/env python3
"""S2.3 step-1 aggregator: fragment-CSR vs v2 on the SAME emitted base.

Reads Data/phase_profile/etapa_b/s23/t{1,12}/raw/{m1,canary_0,canary_1}.txt
and writes Data/phase_profile/etapa_b/s23/aggregate.txt.

Policy (same as s22b_agg.py):
  - rep 0 discarded; medians over 2 usable reps x 3 interleaved samples
    (= 6 samples per builder per matrix);
  - seq 1..6 = MM, H, S, Srev, Stot, result (LHRR rep order; seq 1/2 must
    self-identify as MM/H through their probe tag -- asserted);
  - FRAG* lines ONLY: while PHASE_FRAGAB runs, the PHASEPROFILE ledger is
    contaminated by the extra measurement builds (documented in README);
  - gates: ABKNOBS builder=v2 + fragab=1, pipeline=lhrr, every FRAGABCHECK
    struct=ok with val_diff=0 and col_mismatch=0, >=4 samples per builder on
    every non-rejected matrix, both canaries present, and every matrix
    yields exactly 6 ABBA pairs per T (PAIRS section below).
A C19 reject (is_set inside a duplicate group) is DATA, not a gate failure:
it is reported in the table as REJECT(reason).
"""
import os
import statistics
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "..", "Data", "phase_profile", "etapa_b", "s23")
NAMES = {1: "MM", 2: "H", 3: "S", 4: "Srev", 5: "Stot", 6: "result"}
fails = []
out = []


def w(s=""):
    out.append(s)


def kv(line):
    d = {}
    for f in line.split(",")[1:]:
        if "=" in f:
            k, v = f.split("=", 1)
            d[k] = v
    return d


def rd(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def canary_ms(lines):
    for l in lines or []:
        if l.startswith("REP,1,"):
            return float(l.split(",")[2])
    return None


def stage(t):
    raw = os.path.join(ROOT, "t%d" % t, "raw")
    lines = rd(os.path.join(raw, "m1.txt"))
    if lines is None:
        w("T=%d: MISSING %s/m1.txt" % (t, raw))
        fails.append("T=%d missing run" % t)
        return
    c0 = canary_ms(rd(os.path.join(raw, "canary_0.txt")))
    c1 = canary_ms(rd(os.path.join(raw, "canary_1.txt")))
    if c0 is None or c1 is None:
        fails.append("T=%d missing canary" % t)
    w("=== T=%d ===" % t)
    runs = [l for l in lines if l.startswith("RUN,")]
    if runs and "lhrr" not in runs[0]:
        fails.append("T=%d pipeline != lhrr" % t)
    for k in [l for l in lines if l.startswith("ABKNOBS,")]:
        d = kv(k)
        if d.get("builder") != "v2":
            fails.append("T=%d builder=%s (want v2)" % (t, d.get("builder")))
        if d.get("fragab") != "1":
            fails.append("T=%d fragab!=1" % t)
        w("  " + k.strip())
    # ---- probes: contract verdict per seq (rep 0 discarded) -----------
    probes = {}
    for l in lines:
        if not l.startswith("FRAGPROBE,"):
            continue
        p = l.split(",")
        rep, seq, tag = int(p[1]), int(p[2]), p[3]
        if rep == 0:
            continue
        d = kv(l)
        pr = probes.setdefault(seq, {"tag": tag, "v": set(), "emitted": 0,
                                     "band": 0, "order": 0, "dupes": 0,
                                     "isset": 0})
        pr["v"].add(d.get("verdict", "?"))
        pr["emitted"] += int(d.get("emitted", 0))
        pr["band"] += int(d.get("band_viol", 0))
        pr["order"] += int(d.get("order_viol", 0))
        pr["dupes"] += int(d.get("dupes", 0))
        pr["isset"] += int(d.get("is_set", 0))
    for seq in sorted(probes):
        pr = probes[seq]
        want = NAMES.get(seq, "?")
        if seq in (1, 2) and pr["tag"] != want:
            fails.append("seq %d self-tag %s != %s" % (seq, pr["tag"], want))
        w("  probe seq %d %-7s tag=%-6s emitted=%d band=%d order=%d "
          "dupes=%d is_set=%d verdict=%s"
          % (seq, want, pr["tag"], pr["emitted"], pr["band"], pr["order"],
             pr["dupes"], pr["isset"], "/".join(sorted(pr["v"]))))
    missing = [NAMES[s] for s in NAMES if s not in probes]
    if missing:
        fails.append("T=%d missing probes for %s" % (t, missing))
    # ---- charged normalization / rejects ------------------------------
    preps, rejects = {}, {}
    for l in lines:
        if l.startswith("FRAGPREP,"):
            p = l.split(",")
            if int(p[1]) != 0:
                preps.setdefault(int(p[2]), []).append(
                    kv(l).get("prep_ms", "0"))
        elif l.startswith("FRAGREJECT,"):
            p = l.split(",")
            if int(p[1]) != 0:
                rejects[int(p[2])] = l.split("reason=")[-1]
    # ---- interleaved samples ------------------------------------------
    samples = {}
    slots = {}   # (rep, seq, k) -> (builder, ms): ABBA emission order
    for l in lines:
        if not l.startswith("FRAGAB,"):
            continue
        p = l.split(",")
        rep, seq, builder = int(p[1]), int(p[2]), p[4]
        if rep == 0:
            continue
        d = kv(l)
        samples.setdefault((seq, builder), []).append(float(d["ms"]))
        slots[(rep, seq, int(d.get("sample", -1)))] = (builder,
                                                       float(d["ms"]))
    # ---- structure/value checks ---------------------------------------
    checks = {}
    for l in lines:
        if not l.startswith("FRAGABCHECK,"):
            continue
        p = l.split(",")
        rep, seq = int(p[1]), int(p[2])
        if rep == 0:
            continue
        d = kv(l)
        ok = (d.get("struct") == "ok" and d.get("val_diff") == "0"
              and d.get("col_mismatch") == "0")
        checks.setdefault(seq, 0)
        checks[seq] += 1
        if not ok:
            fails.append("T=%d CHECK FAIL: %s" % (t, l))
    for seq in sorted(NAMES):
        if seq not in rejects and checks.get(seq, 0) < 2:
            fails.append("T=%d seq %d: %d checks (<2)"
                         % (t, seq, checks.get(seq, 0)))
    # ---- per-matrix table ---------------------------------------------
    w("  %-7s %14s %8s %14s %12s %9s  %s"
      % ("matrix", "frag_build_ms", "prep_ms", "frag_total_ms",
         "v2_ms", "tot/v2", "mode"))
    for seq in sorted(NAMES):
        name = NAMES[seq]
        frag = samples.get((seq, "frag"), [])
        v2 = samples.get((seq, "v2"), [])
        prep = med([float(x) for x in preps.get(seq, [])]) \
            if seq in preps else 0.0
        if seq in rejects:
            mode = "REJECT(%s)" % rejects[seq]
        elif seq in preps:
            mode = "norm"
        else:
            mode = "direct"
        if not v2:
            fails.append("T=%d seq %d: no v2 samples" % (t, seq))
        elif len(v2) < 4:
            fails.append("T=%d seq %d: %d v2 samples (<4)"
                         % (t, seq, len(v2)))
        if seq not in rejects:
            if not frag:
                fails.append("T=%d seq %d: no frag samples" % (t, seq))
            elif len(frag) < 4:
                fails.append("T=%d seq %d: %d frag samples (<4)"
                             % (t, seq, len(frag)))
        fm = med(frag)
        ftot = (fm + prep) if frag else float("nan")
        vm = med(v2)
        ratio = (ftot / vm) if (frag and v2 and vm) else float("nan")
        w("  %-7s %14.1f %8.1f %14.1f %12.1f %8.2fx  %s"
          % (name, fm if frag else float("nan"), prep,
             ftot, vm, ratio, mode))
    # ---- paired samples: adjacent ABBA slots (method fixed in README) ----
    # Pairs are (k0,k1),(k2,k3),(k4,k5) WITHIN each usable rep: consecutive
    # emission slots of phase_ab.hpp:296-315 (natural first, then alternate),
    # so each pair holds exactly one frag and one v2 sample taken back to
    # back in wall-clock order (drift-controlled). ratio = frag/v2 < 1 =>
    # fragment arm faster in that pair. Summary side(): ±5% dead band
    # (0.95-1.05) = parity, matching the S2.3 reading of 0.97-0.99x.
    w("  PAIRS slots k=(0,1)(2,3)(4,5)/rep; ratio=frag/v2; n=6 PROVISIONAL")
    w("    %-7s %4s %2s %9s %9s %7s  %s"
      % ("matrix", "pair", "rp", "frag_ms", "v2_ms", "ratio", "winner"))
    for seq in sorted(NAMES):
        rows = []
        for rp in sorted({k[0] for k in slots if k[1] == seq}):
            for pk in (0, 2, 4):
                a, b = slots.get((rp, seq, pk)), slots.get((rp, seq, pk + 1))
                if not a or not b or a[0] == b[0]:
                    continue
                fm, vm = (a[1], b[1]) if a[0] == "frag" else (b[1], a[1])
                rows.append((rp, pk // 2 + 1, fm, vm, fm / vm))
        if seq in rejects:
            continue
        if len(rows) != 6:
            fails.append("T=%d seq %d: %d ABBA pairs (!=6)"
                         % (t, seq, len(rows)))
            continue
        for rp, pr, fm, vm, r in rows:
            w("    %-7s %4d %2d %9.3f %9.3f %7.3f  %s"
              % (NAMES[seq], pr, rp, fm, vm, r, "frag" if r < 1.0 else "v2"))
        wins = sum(1 for x in rows if x[4] < 1.0)
        mp = med([x[4] for x in rows])
        rom = med([x[2] for x in rows]) / med([x[3] for x in rows])

        def side(x):
            return "frag" if x < 0.95 else ("v2" if x > 1.05 else "parity")
        note = "" if seq not in preps else " (frag raw, prep excluded)"
        w("    summary %s: W-L=%d-%d med_pair=%.3f [%s] "
          "ratio_of_medians=%.3f [%s] agree=%s%s"
          % (NAMES[seq], wins, 6 - wins, mp, side(mp), rom, side(rom),
             "yes" if side(mp) == side(rom) else "NO", note))
    w("")
    nchecks = sum(checks.values())
    w("  checks (struct identical + values <=1e-6 rel): %d" % nchecks)
    w("  DRIFT canary rep1: canary_0=%s canary_1=%s"
      % (c0, c1))
    if c0 and c1:
        w("    drift=%+.2f%%" % (100.0 * (c1 - c0) / c0))
    w("")


def main():
    any_run = False
    for t in (1, 12):
        if os.path.exists(os.path.join(ROOT, "t%d" % t, "raw", "m1.txt")):
            stage(t)
            any_run = True
    if not any_run:
        print("no runs found under %s" % ROOT, file=sys.stderr)
        return 2
    w("GATE: " + ("PASS" if not fails else "FAIL"))
    for f in fails:
        w("  FAIL: " + f)
    text = "\n".join(out) + "\n"
    dest = os.path.join(ROOT, "aggregate.txt")
    with open(dest, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(text)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())