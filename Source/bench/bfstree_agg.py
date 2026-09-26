#!/usr/bin/env python3
"""bfstree_agg.py -- aggregate bench_bfstree campaign logs into markdown tables.

Usage:  python3 bfstree_agg.py [log ...]
        (default: /tmp/bfstree_campaign_{0,1,2,3}.log, missing ones skipped)

Input (run_bfstree_campaign.sh): every measured run is introduced by a
  POINT=<i> N=<n> L=<l> k=<k> RUN=<mode>
tag line followed by the bench key=value output (MODE=, DENSITY=, T_*_MS=,
RSS_KB=, CK_W=, CK_FINAL=, BITFINAL=, POS_EQUAL=, P1..P20, T_INV_MS=, F6A_*,
ATTR_*).

Output: markdown tables for the closing README -- run table per point, the
bit-exactness CK chain check, the F6a gate, the E limit sweep and the Eattr
W-level error attribution.

Caveat encoded in the layout: times are single-run wall clock under
OMP_WAIT_POLICY=PASSIVE on WSL2, where session drift is ~15-20%. Cross-point /
cross-session numbers are indicative; the usable deltas are within a point,
where modes run back to back.
"""
import os
import re
import sys
from collections import OrderedDict

DEFAULT = [f"/tmp/bfstree_campaign_{i}.log" for i in range(4)]

RUN_RE = re.compile(r"POINT=(\d+)\s+N=(\d+)\s+L=(\d+)\s+k=(\d+)\s+RUN=(\S+)")
KV_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def parse(path):
    """Return one dict per run block, in file order (first value wins per key)."""
    blocks, cur = [], None
    with open(path, errors="replace") as fh:
        for line in fh:
            m = RUN_RE.search(line)
            if m:
                cur = {"point": int(m.group(1)), "N": int(m.group(2)),
                       "L": int(m.group(3)), "k": int(m.group(4)),
                       "mode": m.group(5), "B": "-", "limit": "-"}
                blocks.append(cur)
                continue
            if cur is None:
                continue
            if line.startswith("MODE="):
                kv = dict(KV_RE.findall(line))
                cur["B"] = kv.get("B", "-")
                cur["limit"] = kv.get("limit", "-")
                continue
            for key, val in KV_RE.findall(line):
                if key not in ("POINT", "N", "L", "k", "RUN"):
                    cur.setdefault(key, val)
    return blocks


def fnum(block, key):
    try:
        return float(block[key])
    except (KeyError, ValueError):
        return None


def mb(block):
    v = fnum(block, "RSS_KB")
    return f"{v / 1024:.0f}" if v else "-"


def runs_table(blocks):
    print("| mode | B | limit | T_TOTAL | speedup vs A | T_F5 | T_F6 | T_F7 "
          "| T_F9 | RSS MB | CK_FINAL | bit-exact |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for b in blocks:
        tot = fnum(b, "T_TOTAL_MS")
        ref = b.get("_ref_total")
        ratio = f"{ref / tot:.2f}x" if ref and tot else "-"
        if tot is None:            # gate modes (F6a, Eattr): no phase timing
            print(f"| {b['mode']} | - | - | gate-only | - | - | - | - | - | - "
                  f"| - | see gate table |")
            continue
        ck = b.get("CK_FINAL", "-")
        bit = "YES" if b.get("BITFINAL") == "YES" else (
            "CK-chain" if ck != "-" else "-")
        print(f"| {b['mode']} | {b['B']} | {b['limit']} | "
              f"{(tot or 0)/1000:.2f} s | {ratio} | "
              f"{(fnum(b, 'T_F5_MS') or 0)/1000:.2f} | "
              f"{(fnum(b, 'T_F6_MS') or 0)/1000:.2f} | "
              f"{(fnum(b, 'T_F7_MS') or 0)/1000:.2f} | "
              f"{(fnum(b, 'T_F9_MS') or 0)/1000:.2f} | {mb(b)} | "
              f"`{(ck[-8:] if ck != '-' else '-')}` | {bit} |")


def chain_check(blocks):
    print("\n**Bit-exactness chain.** CK_FINAL grouped over the exact modes "
          "(A, A2, A3, A4):\n")
    exact = [b for b in blocks if b["mode"] in ("A", "A2", "A3", "A4")]
    cks = OrderedDict()
    for b in exact:
        cks.setdefault(b.get("CK_FINAL", "-"), []).append(
            f"{b['mode']}(B={b['B']})")
    for ck, modes in cks.items():
        print(f"- `{ck}` <- {', '.join(modes)}")
    ok = len(cks) == 1 and "-" not in cks
    print(f"\nCHECK: {'**unique CK_FINAL -> chain bit-exact**' if ok else 'MISMATCH or incomplete'}")
    bits = [b["mode"] for b in exact if b.get("BITFINAL") == "YES"]
    if bits:
        print(f"\nBITFINAL=YES against the golden reference for: "
              f"{', '.join(bits)}")


def f6a_table(blocks):
    rows = [b for b in blocks if b["mode"] == "F6a"]
    if not rows:
        return
    print("\n**F6a gate** (algebraic colsum vs legacy colsum, cell by cell):\n")
    print("| N | BIT | GATE_OK | BEYOND | MAXREL | MAXABS |")
    print("|---|---|---|---|---|---|")
    for b in rows:
        print(f"| {b.get('F6A_N', b['N'])} | {b.get('BIT', '-')} | "
              f"{b.get('GATE_OK', '-')} | {b.get('BEYOND', '-')} | "
              f"{b.get('MAXREL', '-')} | {b.get('MAXABS', '-')} |")


def e_table(blocks):
    rows = [b for b in blocks if b["mode"] == "E"]
    if not rows:
        return
    print("\n**E (native SpGEMM) limit sweep** -- `limit` = max entries kept "
          "per W row:\n")
    print("| limit | T_TOTAL | POS_EQUAL | P@1 | P@5 | P@10 | P@20 | NNZ_W |")
    print("|---|---|---|---|---|---|---|---|")
    for b in rows:
        print(f"| {b['limit']} | {(fnum(b, 'T_TOTAL_MS') or 0)/1000:.2f} s | "
              f"{b.get('POS_EQUAL', '-')} | {b.get('P1', '-')} | "
              f"{b.get('P5', '-')} | {b.get('P10', '-')} | "
              f"{b.get('P20', '-')} | {b.get('NNZ_W', '-')} |")


def attr_table(blocks):
    rows = [b for b in blocks if b["mode"] == "Eattr"]
    if not rows:
        return
    print("\n**Eattr W-level error attribution** (share of the N^2 cells, "
          "legacy W vs SpGEMM W):\n")
    print("| limit | bit-equal | 1e-9 filter | float-round within gates | "
          "float-round beyond gates | pruned | spurious | maxrel beyond |")
    print("|---|---|---|---|---|---|---|---|")
    for b in rows:
        n2 = fnum(b, "ATTR_CELLS") or (fnum(b, "N") or 1) ** 2

        def pct(key):
            v = fnum(b, key)
            return f"{100.0 * v / n2:.2f}%" if v is not None else "-"
        print(f"| {b['limit']} | {pct('ATTR_BIT_EQ')} | "
              f"{pct('ATTR_FILTER')} | {pct('ATTR_FLOAT_OK')} | "
              f"{pct('ATTR_FLOAT_BEYOND')} | {pct('ATTR_PRUNED')} | "
              f"{pct('ATTR_SPURIOUS')} | "
              f"{b.get('ATTR_MAXREL_BEYOND', '-')} |")


def main():
    paths = [p for p in (sys.argv[1:] or DEFAULT) if os.path.exists(p)]
    if not paths:
        print("no campaign logs found", file=sys.stderr)
        return 2
    for path in paths:
        blocks = parse(path)
        if not blocks:
            continue
        a_tot = next((fnum(b, "T_TOTAL_MS") for b in blocks
                      if b["mode"] == "A"), None)
        if a_tot:
            for b in blocks:
                b["_ref_total"] = a_tot
        b0 = blocks[0]
        print(f"\n## POINT {b0['point']}: N={b0['N']} L={b0['L']} "
              f"k={b0['k']}  (`{os.path.basename(path)}`)\n")
        runs_table(blocks)
        chain_check(blocks)
        f6a_table(blocks)
        e_table(blocks)
        attr_table(blocks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
