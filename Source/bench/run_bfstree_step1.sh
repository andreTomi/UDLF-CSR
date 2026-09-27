#!/usr/bin/env bash
# run_bfstree_step1.sh -- Passo 1 do fechamento do port BFSTree: MEDIR o que a
# Etapa 4 projetou.
#
# O que este driver decide, com medicao e nao com extrapolacao:
#
#  (1) rho estrutural de W em DADOS REAIS (TOUCH/TOUCHRAW). Todo rho das Etapas
#      2-4 veio de listas sinteticas (gen_rklists embaralha permutacoes
#      aleatorias), e o modelo de memoria do port depende de rho. TOUCH varre o
#      suporte estrutural de F5 (blocos l1xl1, l2xl2, l1xl2) num bitmap de N^2
#      bits sem materializar W; TOUCHRAW idem sobre as listas cruas e sem
#      alocar nenhuma matriz N^2 -- por isso roda no N de producao (72000) aqui.
#      Validacao: W >= 0 (todo incremento e produto de correlacoes rbo >= 0),
#      logo suporte estrutural == suporte nao-zero: TOUCH_RHO tem que bater com
#      o DENSITY medido nos brutos da campanha; se nao bater, o censo esta
#      errado e nada aqui pode ser usado. TOUCHRAW vs TOUCH mede se o re-sort
#      de F1/F2 altera o suporte -- e o que autoriza (ou nao) usar TOUCHRAW em
#      N=72000, onde TOUCH nao caberia.
#
#  (2) pico de RSS medido A (legado, duas matrizes densas) vs A4 (uma matriz
#      densa + transiente de emissao O(N*L)) no MESMO ponto e binario. A
#      campanha FAST nunca rodou o legado em L=1400, entao a comparacao que
#      decide o objetivo original ("cabe onde o denso nao cabia?") era uma
#      projecao, nao uma medicao.
#
#  (3) contabilidade de bytes da emissao do A4 (A4_TRIPS / A4_CSR_BYTES) e a
#      traca de pico (BFSTREE_MEMTRACE), que separa o termo persistente O(N^2)
#      do transiente O(N*L).
#
# Regras de sessao (as mesmas das Etapas 1-4): um processo por run
# (ru_maxrss limpo por variante); OMP_WAIT_POLICY=PASSIVE; runs que medem
# RSS/tempo NUNCA em paralelo (disputa por memoria e LLC invalida o numero) --
# por tudo isso aqui e sequencial; cada run deixa um log proprio e o agregador
# (bfstree_step1_agg.py) regenera as tabelas a partir dos brutos.
#
# Uso: bash Source/bench/run_bfstree_step1.sh <estagio> [rkLists.txt]
#   touch_synth | real_rho | real_a4 | leg_p3 | cross  | all
#
# Build (WSL, repo root):
#   g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
#       Source/bench/bench_bfstree.cpp -o /tmp/bb_step1
set -u
set -o pipefail

BIN=${BIN:-/tmp/bb_step1}
OUT=${OUT:-Data/phase_profile/bfstree_step1}
RK=${2:-Datasets/aloi/rkLists.txt}
STAGE=${1:-all}
THREADS=${THREADS:-$(nproc)}

[ -x "$BIN" ] || { echo "missing $BIN (build it first)"; exit 2; }
mkdir -p "$OUT"

# Um run por processo; rc registrado em runs.out para auditoria do lote.
run() {  # run <tag> <mode> <N> <L> <k> <seed> <B> <limit> [dataspec] [refpath]
    local tag=$1 mode=$2 N=$3 L=$4 k=$5 seed=$6 B=$7 lim=$8 data=${9:-}
    local ref=${10:--}
    local log="$OUT/${tag}.log"
    local args=("$mode" "$N" "$L" "$k" "$seed" "$B" "$lim")
    [ -n "$data" ] && args+=("$ref" "$data")
    echo "tag=$tag mode=$mode N=$N L=$L k=$k seed=$seed B=$B lim=$lim ref=$ref data=${data:-synth} start=$(date -Is)" \
        >>"$OUT/runs.out"
    OMP_NUM_THREADS=$THREADS OMP_WAIT_POLICY=PASSIVE BFSTREE_MEMTRACE=1 \
        "$BIN" "${args[@]}" >"$log" 2>&1
    echo "tag=$tag rc=$? end=$(date -Is)" | tee -a "$OUT/runs.out"
}

case "$STAGE" in
touch_synth)
    # Suporte estrutural nos pontos da campanha; o DENSITY correspondente ja
    # esta em Data/phase_profile/bfstree_close/raw -- o agregador faz o par.
    run touch_p0_synth   TOUCH     5000  100 20 42 0 0
    run touchraw_p0_synth TOUCHRAW 5000  100 20 42 0 0
    run touch_p1_synth   TOUCH    10000  100 12 42 0 0
    run touchraw_p1_synth TOUCHRAW 10000 100 12 42 0 0
    run touch_p2_synth   TOUCH     5000  100 10 42 0 0
    run touchraw_p2_synth TOUCHRAW 5000  100 10 42 0 0
    run touch_p3_synth   TOUCH    13000 1400 20 42 0 0
    run touchraw_p3_synth TOUCHRAW 13000 1400 20 42 0 0
    ;;
real_rho)
    # O numero que decide A4-vs-A5 antes de qualquer run grande: rho real.
    # TOUCHRAW nao aloca matriz N^2 -> vai ao N de producao neste host.
    run touchraw_p4_real_72k TOUCHRAW 72000 1400 20 0 0 0 "real:$RK"
    run touchraw_p5_real_24k TOUCHRAW 24000 1400 20 0 0 0 "real:$RK"
    run touch_p5_real_24k    TOUCH    24000 1400 20 0 0 0 "real:$RK"
    ;;
leg_p3)
    run a_leg_p3_synth A  13000 1400 20 42 256 13000
    run a4_p3_synth    A4 13000 1400 20 42 256 13000
    ;;
cross)
    # Crossover de pico de RSS entre legado (2 matrizes n^2) e A4 (1 matriz +
    # transiente de emissao). Modelo: o rc da F7 so tem L entradas por linha
    # (A4_CSR_NNZ = N*L exato nos brutos), entao o que o legado desperdica e a
    # segunda matriz inteira (4 B/celula, independente de rho) e o que A4 paga de
    # volta e o buffer de tripletos (12 B x N x L) mais o CSR (8 B x rho x N x L).
    # Razao = 12L/(4N) + 8*rho*L/N -> em rho=1 (sintetico, pior caso) N* ~ 12*L.
    # Medido (pico MB, L=1400): N=13000 A=1408 / A4=1579 (1.122); N=18000
    # A=2651 / A4=2546 (0.960). Interpolando, N* ~ 16.8k = 12*L, como previsto.
    # O ponto em rho real (~2%) sai do estagio real_bitex, onde o legado tambem
    # roda com MEMPROBE sobre listas do ALOI.
    run a_leg_p6_synth  A  18000 1400 20 42 256 18000
    run a4_p6_synth     A4 18000 1400 20 42 256 18000
    run a4_p7_synth     A4 24000 1400 20 42 256 24000
    ;;
real_bitex)
    # Bit-exactness do port sobre ESTRUTURA REAL. Toda a equivalencia bit-a-bit
    # das Etapas 1-4 foi medida com listas sinteticas; o port nunca foi comparado
    # com o legado denso sobre a correlacao de posto do ALOI. O golden e gravado
    # pelo proprio legado (mode ref) sobre listas reais e A/A4/A2 tem que
    # reproduzir CK_FINAL identico + BITFINAL=YES -- o que tambem prova que o
    # carregador real e deterministico entre processos (sem isso, BITFINAL seria
    # NO por construction).
    #
    # Contrato de entrada (medido, nao escolhido): filtrar id<N das 7200 listas
    # deixa ~N/10 sobreviventes por linha e o minimo cai abaixo de L=1400 ate em
    # N=18000 (rows_short=503, min_survivors=1171 no log de ref_p8_real_18k de
    # 22:58; em N=13000 foram 9567 linhas, minimo 881). O carregador recusa
    # (rc=5) em vez de preencher com lixo, entao o maior L redondo valido em
    # N=18000 e 1150. Em N=24000 o L=1400 cabe (min_survivors=1490), mas o legado
    # la pede 2 x 24000^2 x 4 B = 4.6 GB e a maquina deste passo tem ~6 GB
    # disponiveis: a medicao sairia em swap e nao valeria como pico de RSS.
    #
    # Subproduto: estes runs carregam MEMPROBE, entao a dupla a_leg_p8/a4_p8 da a
    # mesma razao de pico do estagio cross, porem com rho real (~2%).
    REF="$OUT/ref_p8_real_18k_l1150.bin"
    run ref_p8_real_18k     ref 18000 1150 20 0 0   18000 "real:$RK" "$REF"
    run a_leg_p8_real_18k   A   18000 1150 20 0 0   18000 "real:$RK" "$REF"
    run a4_p8_real_18k      A4  18000 1150 20 0 256 18000 "real:$RK" "$REF"
    run a2_p8_real_18k      A2  18000 1150 20 0 256 18000 "real:$RK" "$REF"
    ;;
real_sparse)
    # A3/A2 sobre ESTRUTURA REAL. E aqui que o rho de 1-2% vira memoria: nesses
    # modos o rc da F7 sai como CSR e F8/F9 rodam esparsos, entao o que decide o
    # CSR da saida compensar ou nao e rc.nnz(), e a campania das Etapas 2-4 mediu
    # isso com lista sintetica, onde rc sai denso (rho~1) e o CSR so adiciona
    # overhead. rc.nnz() medido em dados reais + pico RSS medido sao o que falta
    # para extrapolar N=72000 com todos os termos reais.
    run a3_p5_real_24k A3 24000 1400 20 0 256 24000 "real:$RK"
    run a2_p5_real_24k A2 24000 1400 20 0 256 24000 "real:$RK"
    ;;
all)
    for s in touch_synth real_rho leg_p3 cross real_bitex real_sparse real_a4; do bash "$0" "$s" "$RK"; done
    ;;
*)
    echo "unknown stage '$STAGE'"; exit 2
    ;;
esac
echo "STEP1_STAGE_${STAGE}_DONE"
