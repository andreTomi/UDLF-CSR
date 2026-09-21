#!/usr/bin/env python3
# fix_crlf.py -- copy text repo files to /tmp with CRLF stripped (WSL 9p keeps
# serving a stale no-CRLF page after in-place edits, which corrupts bash).
# Usage: python3 Source/bench/fix_crlf.py <file> [<file> ...]
import sys

for path in sys.argv[1:]:
    data = open(path, "rb").read().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    out = "/tmp/" + path.replace("\\", "/").split("/")[-1]
    with open(out, "wb") as f:
        f.write(data)
    print(f"{path} -> {out} ({len(data)} bytes)")
