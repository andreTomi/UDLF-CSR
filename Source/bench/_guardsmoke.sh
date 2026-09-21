#!/usr/bin/env bash
# _guardsmoke.sh -- prove the affinity guard actually aborts on a restricted caller.
set -u
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
echo "### syntax check"
bash -n /tmp/run_etapa_a.sh && echo "syntax OK"
echo
echo "### real dataset binary"
ls -la /tmp/udlf_rk_aloi_N72000_L400.bin 2>&1
echo
echo "### guard smoke: restricted caller (taskset -c 0,1) must MASKFAIL+exit 90"
taskset -c 0,1 bash /tmp/run_etapa_a.sh /tmp/smoke_guard build 2>&1 | tail -2
taskset -c 0,1 bash /tmp/run_etapa_a.sh /tmp/smoke_guard curve_fast 2>&1 | tail -4
echo "exit_of_restricted_stage=$?"
echo
echo "### guard smoke: free caller must pass"
bash /tmp/run_etapa_a.sh /tmp/smoke_guard build 2>&1 | tail -1
