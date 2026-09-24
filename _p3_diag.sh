#!/bin/bash
# _p3_diag.sh <ab12|ab1|abcprr> -- Fase 0 do diagnostico S2.4: A/B same-session
# com ABBA intercalado (pre=new=new=pre por ciclo), >=5 reps usaveis por braco,
# canario T=12 por bloco (deriva da sessao) e evidencia crua em
# Data/phase_profile/etapa_b/s24/raw_<mode>/. Agrega com _p3_diag.py.
#   pre = /home/andre/udlf_bin/bp_pre_p2 (commit 1f56845, -DNDEBUG)
#   new = /home/andre/udlf_bin/bp_s23    (arvore de trabalho, S2.3 p2)
# Protocolo igual ao _p2_ab.sh: warm-ups descartados, rep 0 de cada bloco
# descartado, taskset + OMP/PHASE_THREADS fixos.
set -e
R=/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR
D=$R/Data/phase_profile/etapa_b/s24
PREBIN=${PREBIN:-/home/andre/udlf_bin/bp_pre_p2}
NEWBIN=${NEWBIN:-/home/andre/udlf_bin/bp_s23}
BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
N=72000; L=400; K=10; LIMIT=400
MODE=${1:?usage: _p3_diag.sh <ab12|ab1|abcprr>}
case "$MODE" in
  ab12)   MASK=0-11; T=12; PIPE=lhrr; REPS=3; CYCLES=3 ;;
  ab1)    MASK=0;    T=1;  PIPE=lhrr; REPS=3; CYCLES=3 ;;
  abcprr) MASK=0-11; T=12; PIPE=cprr; REPS=5; CYCLES=3 ;;
  *) echo "bad mode"; exit 2 ;;
esac
# OUT sobregravavel (Fase 1: OUT=$D/raw_<mode>_f1 preserva a evidencia Fase 0;
# leitura: python3 _p3_diag.py <mode> _f1)
OUT=${OUT:-$D/raw_$MODE}
mkdir -p "$OUT"
cd "$R"

run_arm() { # <bin> <tag>
  taskset -c "$MASK" env OMP_NUM_THREADS=$T PHASE_THREADS=$T \
    "$1" real "$BIN" $N $L $K $LIMIT "$PIPE" $REPS > "$OUT/$2.txt" 2>&1
}
canary() { # <tag> -- sonda de deriva da sessao (T=12, bin new fixo)
  # Fonte unica do workload: Source/bench/canary.sh (mult w3, piso >=1s).
  # O `mult w1` inline foi a regressao do S1-bis reincidente (H3).
  # fix_crlf copia canary.sh para /tmp (repo e CRLF/9p); fallback ao repo.
  if [ -f /tmp/canary.sh ]; then source /tmp/canary.sh; else source "$R/Source/bench/canary.sh"; fi
  canary_run "$NEWBIN" "$OUT/$1.txt"
  canary_check "$OUT/$1.txt"
}

echo "=== warm-ups (descartados) ==="
run_arm "$PREBIN" warm_pre
run_arm "$NEWBIN" warm_new
canary canary_0

for c in $(seq 1 $CYCLES); do
  echo "=== ABBA cycle $c: pre,new,new,pre (REPS=$REPS, T=$T, $PIPE) ==="
  run_arm "$PREBIN" c${c}_pre_a
  run_arm "$NEWBIN" c${c}_new_a
  run_arm "$NEWBIN" c${c}_new_b
  run_arm "$PREBIN" c${c}_pre_b
  canary canary_$c
done

for f in "$OUT"/c*_*.txt; do
  echo "## $(basename "$f" .txt)"
  grep -a -E '^REP,|^DONE,' "$f" | head -8 || true
done
echo "=== P3 DIAG $MODE DONE ==="