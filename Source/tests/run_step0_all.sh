#!/usr/bin/env bash
# run_step0_all.sh -- run the three Step 0 gates together, from a cold start.
#
# One entry point so a reviewer runs the same thing the author ran. The three
# gates are INDEPENDENT (each builds its own binary under its own name), so this
# is a convenience wrapper, not an ordering requirement -- verified by running the
# probes before the gate as well.
#
# Nothing here writes to Source/src, Source/bench or Source/tests except by
# BUILDING test_rfe_golden.cpp; no md5 registry and no existing test is touched.
set -u
ROOT=${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$ROOT" || exit 1

# WSL keeps a stale CRLF-free page for freshly edited files (see bench/README.md
# and bench/fix_crlf.py), so strip before running.
python3 Source/bench/fix_crlf.py \
  Source/tests/step0_check.sh \
  Source/tests/step0_probe_k.sh \
  Source/tests/step0_probe_saturated.sh >/dev/null || exit 1

rc=0
for s in step0_check step0_probe_k step0_probe_saturated; do
  echo "=============================================================="
  echo "== $s"
  echo "=============================================================="
  bash "/tmp/$s.sh" || { echo "!! $s FALHOU"; rc=1; }
  echo
done

if [ "$rc" = 0 ]; then
  echo "RFE STEP0 (3/3 gates): OK"
else
  echo "RFE STEP0: FALHOU" >&2
fi
exit "$rc"