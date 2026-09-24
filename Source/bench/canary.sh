#!/bin/bash
# canary.sh -- FONTE UNICA do canario de deriva de sessao (piso S1-bis).
#
# Os harnesses devem `source` este arquivo em vez de reescrever o canario
# inline. Historico: o workload foi herdado verbatim do template s22b_run.sh
# (`mult w1`, ~64-121 ms por rep) e REGRESSOU 3x por copia de template sem
# troca -- s23_run.sh (corrigido, virou w3), _p2_profile.sh e _p3_diag.sh
# (ambos corrigidos aqui) -- causando GATE canario FAIL em 4/4 sessoes LHRR
# (drift 20-142%, H3). Um workload de <1 s nao separa deriva de sessao de
# ruido amostral (licao S1-bis). NAO reescreva o workload inline.
#
# Uso:
#   source <dir>/canary.sh
#   canary_run <driver> <outfile>     # roda a sonda (T=12 fixo)
#   canary_check <outfile>            # gate: rep >= CANARY_MIN_MS e checksum
#                                     # (aborta via exit!=0; harnesses rodam
#                                     #  com `set -e`)
#
# fix_crlf: repo e CRLF; se este script for sourceado de dentro do repo sem
# passar pelo fix_crlf, o bash pode falhar. O fluxo padrao ja faz:
#   python3 Source/bench/fix_crlf.py Source/bench/canary.sh  # -> /tmp
#   python3 Source/bench/fix_crlf.py _p3_diag.sh              # -> /tmp
#   bash /tmp/_p3_diag.sh abcprr
CANARY_WORKLOAD=${CANARY_WORKLOAD:-w3}       # S1-bis: >= 1 s por rep
CANARY_REPS=${CANARY_REPS:-2}
CANARY_T=${CANARY_T:-12}
CANARY_MASK=${CANARY_MASK:-0-11}
CANARY_MIN_MS=${CANARY_MIN_MS:-1000}         # piso mecanico (licao S1-bis)
CANARY_CKSUM=${CANARY_CKSUM:-8273.428589}    # checksum esperado do mult w3

canary_run() { # <driver> <outfile>
    taskset -c "$CANARY_MASK" \
        env OMP_NUM_THREADS=$CANARY_T PHASE_THREADS=$CANARY_T \
        "$1" mult "$CANARY_WORKLOAD" "$CANARY_REPS" > "$2" 2>&1
}

canary_check() { # <outfile> -- piso de duracao + checksum; exit 1 se falhar
    local f=$1
    if [ ! -f "$f" ]; then
        echo "canary_check: missing file $f" >&2
        return 1
    fi
    grep -a '^REP,' "$f" | awk -F, \
        -v min="$CANARY_MIN_MS" -v ck="$CANARY_CKSUM" -v wl="$CANARY_WORKLOAD" '
        BEGIN { n = 0; rc = 0 }
        {
            ++n
            ms = $3 + 0
            if (ms < min) {
                printf("canary_check: rep %d = %.0f ms < piso %d ms (workload %s)\n", \
                       $2, ms, min, wl) > "/dev/stderr"
                rc = 1
            }
            if ($5 != ck) {
                printf("canary_check: rep %d checksum %s != esperado %s\n", \
                       $2, $5, ck) > "/dev/stderr"
                rc = 1
            }
        }
        END {
            if (n == 0) {
                print "canary_check: no REP lines" > "/dev/stderr"
                rc = 1
            }
            exit rc
        }'
}