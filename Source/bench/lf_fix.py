#!/usr/bin/env python3
"""lf_fix.py -- one-shot hygiene pass for the working tree.

The git blobs are LF but the working tree files ended up CRLF (Windows editors /
9p).  Writing in place through the 9p mount is unreliable (stale page cache), so
this script writes to a sibling temp file and os.replace()s it.

- files with real (CR-insensitive) edits  -> normalized to LF, edits kept
- files whose only difference was CRLF     -> restored from HEAD via git checkout
"""
import os
import subprocess

REPO = "/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR"
LOG = "/tmp/lf_fix.txt"
os.chdir(REPO)


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True).stdout


modified = [l[3:] for l in git("status", "--porcelain").splitlines()
            if l[:2] in (" M", "MM", "M ")]
real = set(git("diff", "--ignore-cr-at-eol", "--name-only").split())

lines = []
for path in modified:
    if path in real:
        with open(path, "rb") as fh:
            data = fh.read()
        clean = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        tmp = path + ".lftmp"
        with open(tmp, "wb") as fh:
            fh.write(clean)
        os.replace(tmp, path)
        lines.append("normalized(LF, edits kept) %s  (%d -> %d bytes)"
                     % (path, len(data), len(clean)))
    else:
        subprocess.run(["git", "checkout", "--", path], check=True)
        lines.append("restored(HEAD)              %s" % path)

with open(LOG, "w") as fh:
    fh.write("\n".join(lines) + "\n")
print("modified=%d  real=%d  restored=%d" % (len(modified), len(real),
                                             len(modified) - len(real)))
