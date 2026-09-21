#!/usr/bin/env python3
"""_a4_check.py -- validate the A4 peak-RSS capture against GNU time %M.

Runs the real harness entry point (run_with_advanced_monitoring) on a child
that allocates ~300 MB, both behind a /usr/bin/time wrapper (the case the
harness normally uses, and the case that produced the flat 11 MB column) and
without any wrapper.  Expected: peak_rss_mb tracks the child's own VmHWM.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, "Test/src")
from exp_runner.execution import run_with_advanced_monitoring  # noqa: E402

CHILD = Path("/tmp/_a4_child.py")
CHILD.write_text(
    "import time\n"
    "buf = bytearray(300 * 1024 * 1024)\n"
    "for i in range(0, len(buf), 4096):\n"
    "    buf[i] = 1\n"
    "hwm = [l for l in open('/proc/self/status') if l.startswith('VmHWM')][0]\n"
    "print('child-self-' + hwm.strip())\n"
    "time.sleep(0.6)\n")


def probe(label, cmd):
    log = Path("/tmp/_a4_%s.log" % label)
    res = run_with_advanced_monitoring(cmd, timeout=180, log_path=log)
    text = log.read_text(errors="replace")
    m = re.search(r"Maximum resident set size \(kbytes\):\s+(\d+)", text)
    tmb = int(m.group(1)) / 1024.0 if m else -1.0
    c = re.search(r"child-self-VmHWM:\s+(\d+)", text)
    cmb = int(c.group(1)) / 1024.0 if c else -1.0
    print("== %s" % label)
    print("   cmd                   : %s" % " ".join(cmd))
    print("   exit/status           : %s / %s" % (res.exit_code, res.status))
    print("   max_rss_mb (legacy)   : %8.1f MB   <- who this samples" % res.max_rss_mb)
    print("   peak_rss_mb (A4)      : %8.1f MB" % res.peak_rss_mb)
    print("   peak_ru_maxrss_mb     : %8.1f MB   (ceiling, monotone)" % res.peak_ru_maxrss_mb)
    print("   child own VmHWM       : %8.1f MB" % cmb)
    print("   GNU time %%M           : %8.1f MB" % tmb)
    return res


probe("shell_wrap", ["sh", "-c", "python3 %s" % CHILD])
probe("taskset_wrap", ["taskset", "-c", "0", "python3", str(CHILD)])
probe("bare", ["python3", str(CHILD)])
print("\nPASS if peak_rss_mb ~= child own VmHWM in every case.")
