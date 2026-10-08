#!/usr/bin/env python3
"""mk_rk_from_leading_id.py -- DERIVED dataset builder (Step 3 / Stage B1).

corel5k/ACC.txt and linnaeus5_resnet50.txt use the format

    line q  ->   q  n1 n2 ... nK

i.e. the FIRST TOKEN IS THE QUERY ID and the query is NOT in its own neighbour
list. ALOI/vegfru rkLists.txt use instead

    line q  ->   q  n1 n2 ... n_{L-1}      (self at index 0, no separate token)

so these two cannot be fed to the parity gate without conversion. This script
performs that conversion EXPLICITLY and records provenance. It is not a silent
repair: the output is a DIFFERENT dataset and is named as derived.

Transform (per row q):
    out[q] = [q] ++ (first L-1 neighbours, q removed if present)

Refuses (exit 5) rather than filling: a row that cannot supply L distinct ids.
"""
import sys, os, hashlib

src, L, out, label = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
rows = []
with open(src, "r") as f:
    for ln, line in enumerate(f):
        t = line.split()
        if not t:
            continue
        q = int(t[0])
        nb = [int(x) for x in t[1:]]
        rows.append((q, nb))

N = len(rows)
print("RKCV src=%s label=%s bytes=%d lines=%d N=%d L=%d" %
      (src, label, os.path.getsize(src), N, N, L))

bad_id = dup = short = self_in_nb = 0
out_rows = []
for q, nb in rows:
    seen = set()
    keep = []
    for v in nb:
        if v == q:
            self_in_nb += 1
            continue
        if v in seen:
            dup += 1
            continue
        seen.add(v)
        keep.append(v)
        if len(keep) == L - 1:
            break
    if len(keep) < L - 1:
        short += 1
        continue
    out_rows.append([q] + keep)

print("RKCV self_in_neighbour_list=%d dup_removed=%d rows_short=%d kept=%d" %
      (self_in_nb, dup, short, len(out_rows)))
print("RKCV self_at_0_after=%d/%d (%.3f%%)" %
      (sum(1 for r in out_rows if r[0] == r[0]), len(out_rows), 100.0))
if short:
    print("RKCV_REFUSED: %d rows cannot supply L-1=%d distinct neighbours" % (short, L - 1))
    sys.exit(5)

with open(out, "w") as f:
    for r in out_rows:
        f.write(" ".join(map(str, r)) + "\n")

md5 = hashlib.md5(open(out, "rb").read()).hexdigest()
srcmd5 = hashlib.md5(open(src, "rb").read()).hexdigest()
print("RKCV wrote=%s rows=%d cols=%d md5=%s" % (out, len(out_rows), L, md5))
with open(out + ".provenance", "w") as f:
    f.write("DERIVED dataset\n")
    f.write("label      = %s\n" % label)
    f.write("source     = %s\n" % src)
    f.write("source_md5 = %s\n" % srcmd5)
    f.write("N          = %d\n" % N)
    f.write("L          = %d  (row width INCLUDING self at index 0 => self + %d neighbours)\n" % (L, L - 1))
    f.write("transform  = strip leading query id; prepend self at index 0; drop duplicates; truncate to L-1 neighbours\n")
    f.write("self_in_neighbour_list_dropped = %d\n" % self_in_nb)
    f.write("duplicates_removed = %d\n" % dup)
    f.write("output_md5 = %s\n" % md5)
print("RKCV provenance -> %s.provenance" % out)
