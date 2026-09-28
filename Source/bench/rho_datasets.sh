#!/usr/bin/env bash
# rho_datasets.sh -- o censo de rho em TRES manifolds, com controle de N.
#
# Por que este script existe: todo rho das Etapas 2-4 veio de lista sintetica
# (gen_rklists embaralha permutacoes aleatorias) e o unico rho real medido
# (Data/phase_profile/bfstree_step1) e do ALOI -- e 13k/18k/24k la sao PREFIXOS
# do mesmo arquivo, ou seja, diversidade de ESCALA, nao de manifold. A premissa
# do A5 ("a producao real e esparsa e o custo de materializar zero e o que dói")
# nunca foi testada fora de uma unica colecao.
#
# Controle necessario (e o que torna o numero legivel): rho e uma fracao sobre
# N^2 e o suporte de uma linha cresce com |inv[a]| ~ N*k/L, entao rho sobe
# quando N cai. Comparar colecoes em N diferentes nao mede manifold, mede N.
# Por isso a grade tem PARES CASADOS no mesmo (N, L, k) -- o par compara
# manifolds -- e pontos de producao em N grande -- o ponto compara escalas.
# Cada exec imprime o que o run EH (pair= / scale=), para o agregador nao
# misturar as duas leituras.
#
# Modos usados:
#   TOUCHRAW  censo estrutural sobre as listas cruas, sem alocar N^2 -> roda no
#             N de producao (72000).
#   TOUCH     censo estrutural depois do re-sort de F1/F2 (a ordem que F5 ve).
#   A         legado denso: imprime NNZ_W medido. Gate medido em 28/09/2026
#             (Data/phase_profile/rho_datasets/README.md): TOUCH == NNZ_W fecha
#             EXATO em corel5k (15052942) e linnaeus (19431216), como nos dois
#             pares-gate da Etapa 1; TOUCHRAW == TOUCH NAO se confirmou nestes
#             datasets (-37% corel5k, -25% linnaeus -- e -1,96% no ALOI 24k):
#             TOUCHRAW e proxy com ERRO MEDIDO, nao identidade, e em rank list
#             de ANN ground-truth o erro nao e pequeno. W >= 0 sem cancelamento,
#             entao TOUCH == NNZ_W (gap zero-por-underflow) segue sendo o gate
#             da politica de zeros do A5.
#
# O contrato de entrada do carregador (rc=4 linha sem self, rc=5 linha com < L
# sobreviventes apos filtrar id<N) e gravado no log de cada run em vez de virar
# assercao deste script: rc e um resultado medido aqui, nao uma expectativa.
# Isso importa porque os rank lists do Corel5k e do Linnaeus vem de ground-truth
# de ANN, onde o self pode nao estar na lista -- e e o contrato que diz se o
# censo nesses datasets vale, ou se so mede o subconjunto que passa.
#
# Uso: bash Source/bench/rho_datasets.sh [estagio]
#   probe | build | aloi_small | aloi_big | pair | newds | big | all
# Variaveis: BIN, OUT, LGRID, THREADS, RK, C5K, LIN, K
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/bb_rho}
OUT=${OUT:-Data/phase_profile/rho_datasets}
FLAGS=${FLAGS:--std=gnu++2b -O2 -ffp-contract=off -fopenmp}
THREADS=${THREADS:-12}
K=${K:-20}
RK=${RK:-Datasets/aloi/rkLists.txt}
C5K=${C5K:-Datasets/corel5k/ACC.txt}
LIN=${LIN:-Datasets/linnaeus/linnaeus5_resnet50.txt}
STAGE=${1:-all}

fail() { echo "RHO FAILED: $*" >&2; exit 1; }
mkdir -p "$OUT" || fail "mkdir $OUT"

probe() {  # probe <nome> <arquivo>  -> N, largura, contrato de self, bytes
    local name=$1 f=$2
    [ -r "$f" ] || fail "dataset $name ausente: $f"
    local n sz
    n=$(wc -l <"$f")                       # 1 passeio completo (mede N de verdade)
    sz=$(stat -c %s "$f")
    {
        echo "ds=$name file=$f N=$n bytes=$sz"
        # Passeio limitado a 200 MB: largura constante + self como primeiro
        # token. O contador de self aqui e uma implementacao INDEPENDENTE do
        # censo do carregador (que imprime self_at0 no log de cada run): os dois
        # tem que bater no mesmo arquivo, senao um dos dois esta errado.
        head -c 200000000 "$f" | awk -v name="$name" '
            { if (NR == 1) w = NF; if (NF != w) bad++; if ($1 != NR - 1) noself++ }
            END { printf "ds=%s probe_lines=%d width=%d linhas_de_largura_diferente=%d primeiro_token_diferente_do_id=%d\n",
                          name, NR, w, bad+0, noself+0 }'
        echo "ds=$name max_L_sem_truncar=<width acima>  # L<=width => rows_short=0 por construcao"
    } >>"$OUT/probe.out" 2>&1 || fail "probe $name"
}

build() {
    echo "build: $CXX $FLAGS -I Source/src Source/bench/bench_bfstree.cpp -o $BIN"
    $CXX $FLAGS -I Source/src Source/bench/bench_bfstree.cpp -o "$BIN" \
        || fail "build do bench_bfstree"
    "$BIN" >"$OUT/usage.out" 2>&1
    [ $? -eq 2 ] || fail "usage rc!=2 -- a interface do bench mudou, confira os argv antes de rodar"
    cp "$OUT/usage.out" "$OUT/usage_expected.txt"
}

# run <tag> <ds> <file> <mode> <N> <L> <role>
# role = que o run EH na leitura agregada (pair:match=<ds>|pair:scale). Um
# processo por run: ru_maxrss e por processo e runs que medem RSS/tempo nunca
# vao em paralelo (regra da casa, ver run_bfstree_step1.sh).
run() {
    local tag=$1 ds=$2 f=$3 mode=$4 N=$5 L=$6 role=$7
    local log="$OUT/${tag}.log"
    echo "tag=$tag ds=$ds mode=$mode N=$N L=$L k=$K B=256 role=$role file=$f start=$(date -Is)" \
        >>"$OUT/runs.out"
    OMP_NUM_THREADS=$THREADS OMP_WAIT_POLICY=PASSIVE \
        "$BIN" "$mode" "$N" "$L" "$K" 0 256 "$N" "" "real:$f" >"$log" 2>&1
    local rc=$?
    echo "tag=$tag rc=$rc end=$(date -Is)" | tee -a "$OUT/runs.out"
    return 0
}

stage_aloi_small() {
    # ALOI em N pequeno: aqui o contrato de sub-corpus mordida (filtra id<N das
    # 7200 listas deixa ~N/10 sobreviventes), entao L fica pequeno e o rc e a
    # medicao. Vale porque da os PARES com Corel5k (N=5000) e Linnaeus (N=8000).
    run aloi_5k_400_raw   aloi "$RK" TOUCHRAW  5000  400 pair:corel5k
    run aloi_5k_400_touch aloi "$RK" TOUCH     5000  400 pair:corel5k
    run aloi_8k_400_raw   aloi "$RK" TOUCHRAW  8000  400 pair:linnaeus
    run aloi_8k_400_touch aloi "$RK" TOUCH     8000  400 pair:linnaeus
    run aloi_8k_400_A     aloi "$RK" A         8000  400 pair:linnaeus
}

stage_aloi_big() {
    # Pontos de escala no manifold ja medido (comparaveis com a Etapa 1).
    run aloi_24k_1400_raw aloi "$RK" TOUCHRAW 24000 1400 scale
    run aloi_72k_1400_raw aloi "$RK" TOUCHRAW 72000 1400 scale
}

stage_newds() {
    # Pares casados com o ALOI no mesmo (N, L, k).
    run c5k_5k_400_raw   corel5k "$C5K" TOUCHRAW 5000 400 pair:aloi
    run c5k_5k_400_touch corel5k "$C5K" TOUCH    5000 400 pair:aloi
    run lin_8k_400_raw   linnaeus "$LIN" TOUCHRAW 8000 400 pair:aloi
    run lin_8k_400_touch linnaeus "$LIN" TOUCH    8000 400 pair:aloi
    # Maior L sem truncar a lista (= largura medida no probe): com lista
    # completa o suporte satura, entao aqui o rho mede "manifold denso".
    run c5k_5k_5000_raw   corel5k "$C5K" TOUCHRAW 5000 5000 pair:none
    run c5k_5k_5000_touch corel5k "$C5K" TOUCH    5000 5000 pair:none
    run c5k_5k_5000_A     corel5k "$C5K" A        5000 5000 gate_nnz
    run lin_8k_4000_raw   linnaeus "$LIN" TOUCHRAW 8000 4000 pair:none
    run lin_8k_4000_touch linnaeus "$LIN" TOUCH    8000 4000 pair:none
    run lin_8k_4000_A     linnaeus "$LIN" A        8000 4000 gate_nnz
}

stage_all() {
    probe corel5k "$C5K"; probe linnaeus "$LIN"; probe aloi "$RK"
    build
    stage_newds
    stage_aloi_small
    stage_aloi_big
}

summary() {
    # Uma linha por run, a partir dos brutos (nada e recriado a memoria).
    : >"$OUT/summary.tsv"
    printf 'tag\tds\tmode\tN\tL\tk\tL_over_N\trc\trho\tsupport\tnnz_w\ttouch_ms\trss_KB\tself_missing\tself_at0\tmin_surv\trows_short\n' >>"$OUT/summary.tsv"
    local tag ds mode N L role rc line rho sup nnz ms rss sm sa mn rs
    while read -r tag ds mode N L role; do
        [ -n "$tag" ] || continue
        local log="$OUT/${tag}.log"
        [ -r "$log" ] || continue
        rc=$(sed -n 's/^tag='"$tag"' rc=\([0-9]*\) .*/\1/p' "$OUT/runs.out" | tail -1)
        rho=$(grep -o 'TOUCH_RHO=[0-9.]*' "$log" | tail -1 | cut -d= -f2)
        sup=$(grep -o 'TOUCH_SUPPORT=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        nnz=$(grep -o 'NNZ_W=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        ms=$(grep -o 'TOUCH_MS=[0-9.]*' "$log" | tail -1 | cut -d= -f2)
        rss=$(grep -o 'RSS_KB=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        sm=$(grep -o 'self_missing=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        sa=$(grep -o 'self_at0=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        mn=$(grep -o 'min_survivors=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        rs=$(grep -o 'rows_short=[0-9]*' "$log" | tail -1 | cut -d= -f2)
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%.4f\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$tag" "$ds" "$mode" "$N" "$L" "$K" \
            "$(awk -v l="$L" -v n="$N" 'BEGIN{printf "%.6f", l/n}')" \
            "${rc:-?}" "${rho:-NA}" "${sup:-NA}" "${nnz:-NA}" "${ms:-NA}" "${rss:-NA}" \
            "${sm:-NA}" "${sa:-NA}" "${mn:-NA}" "${rs:-NA}" >>"$OUT/summary.tsv"
    done < <(sed -n 's/^tag=\([^ ]*\) ds=\([^ ]*\) mode=\([^ ]*\) N=\([^ ]*\) L=\([^ ]*\) k=[^ ]* B=[^ ]* role=\([^ ]*\).*/\1\t\2\t\3\t\4\t\5\t\6/p' "$OUT/runs.out" 2>/dev/null)
    column -t -s $'\t' "$OUT/summary.tsv" 2>/dev/null || cat "$OUT/summary.tsv"
}

case "$STAGE" in
    probe)  probe corel5k "$C5K"; probe linnaeus "$LIN"; probe aloi "$RK" ;;
    build)  build ;;
    pair)   stage_newds ;;
    aloi_small) stage_aloi_small ;;
    aloi_big)   stage_aloi_big ;;
    newds)  stage_newds ;;
    big)    stage_aloi_big ;;
    summary) summary ;;
    all)    stage_all; summary ;;
    *) echo "estagio desconhecido: $STAGE"; exit 2 ;;
esac
echo "RHO_STAGE_${STAGE}_DONE"

