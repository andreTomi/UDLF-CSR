#!/usr/bin/env bash
# _etapa_a_v2_rest.sh -- second half of the unbound re-run: wait for the running
# curve stage set to finish, then run the pipe/mem/real stages into the same
# outdir so the whole Etapa A dataset shares one (correct) binding.
set -u
while pgrep -f "run_etapa_a.sh /tmp/etapa_a_v2" > /dev/null; do sleep 10; done
echo "=== curve stages done at $(date +%H:%M:%S); running pipe/mem/real"
exec env EA_REPS=5 bash /tmp/run_etapa_a.sh /tmp/etapa_a_v2 pipe_w1 pipe_w3 mem real
