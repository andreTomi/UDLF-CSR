#!/usr/bin/env python3
"""Strip CRLF and run a shell gate from the repo root.

Nested PowerShell quoting mangles `\\r` and `$` inside `wsl -e bash -lc`, so the
CRLF normalisation is done here (in python) and the gate is invoked by path.
Usage: python3 _run_gate.py Source/tests/step2_check.sh
"""
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
gate = sys.argv[1]
src = os.path.join(repo, gate)
dst = "/tmp/_gate_run.sh"
d = open(src, "rb").read()
n = d.count(b"\r\n")
open(dst, "wb").write(d.replace(b"\r\n", b"\n"))
print("%s: %d CRLF normalised -> %s" % (gate, n, dst))
env = dict(os.environ)
env["ROOT"] = repo
sys.exit(subprocess.call(["bash", dst], cwd=repo, env=env))