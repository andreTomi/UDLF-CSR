#!/usr/bin/env bash
# run_a5_bitex.sh -- validacao bit-exata do A5 (W POR VALOR) contra o golden
# gravado pelo proprio legado, no protocolo real_bitex da Etapa 1 -- agora nos
# datasets que carregam a motivacao da tese (vegfru/sun397) mais o marco do
# ALOI (N=18000, L=1150) como referencia cruzada do protocolo antigo.
#
# Por que isto existe: o A5 e o unico portador cuja exatidao NAO pode ser
# derivada so por algebra -- o argumento "W >= 0, entao pular termo ausente e
# exato" tem um ponto cego declarado (0.0/0.0 = NaN quando acum[c] == 0, que o
# denso materializa e um CSR nao materializa por omissao). O gate do A5 e
# portanto MEDIDO: CK_FINAL do A5 tem que ser identico ao CK_FINAL do golden do
# legado, celula a celula (BITFINAL=YES POS_EQUAL=1.000000), e o censo
# A5CENSUS tem que fechar com STORED_DIFF=0 e DENSE_NONZERO_MISSING=0.
#
# O golden e gravado por `ref` (legado denso na mesma entrada real) e fica em
# $OUT/*.bin -- gitignored (padrao da casa, ver .gitattributes + .gitignore).
# Custo de memoria por ponto (medido): a densa N^2 de F1/F2 e o termo que manda
# -- 0,68 GB em N=13k, 1,3 GB em N=18k, 2,3 GB em N=24k. Acima disso o host de
# 8 GB nao sustenta e o ponto NAO e rodado (limite medido, ver README).
#
# Uso: bash Source/bench/run_a5_bitex.sh [estagio]   (probe|veg|sun|aloi|all)
# Variaveis: BIN, OUT, THREADS, K, LVEG, LSUN, LALOI, B
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
BIN=${BIN:-/tmp/bb_a5}
OUT=${OUT:-Data/phase_profile/bfstree_a5}
FLAGS=${FLAGS:--std=gnu++2b -O2 -ffp-contract=off -fopenmp}
THREADS=${THREADS:-12}
K=${K:-20}
B=${B:-256}
VEG=${VEG:-Datasets/vegfru/rkLists.txt}
SUN=${SUN:-Datasets/sun397/sun397_resnet152.txt}
RK=${RK:-Datasets/aloi/rkLists.txt}
LVEG=${LVEG:-40}      # min_survivors medido em 13k: 46
LSUN=${LSUN:-25}      # min medido: 17 em 13k (invalido) e 105 em 24k
LALOI=${LALOI:-1150}  # L valido do marco N=18000 (Etapa 1, min 1171)
STAGE=${1:-all}

fail() { echo "A5 FAILED: $*" >&2; exit 1; }
mkdir -p "$OUT" || fail "mkdir $OUT"

build() {
    echo "build: $CXX $FLAGS -I Source/src Source/bench/bench_bfstree.cpp -o $BIN"
    $CXX $FLAGS -I Source/src Source/bench/bench_bfstree.cpp -o "$BIN" \
        || fail "build do bench_bfstree"
}

# run <tag> <mode> <N> <L> <seed> <B> <file> [ref]
# B=0 no modo ref (o legado nao usa o knob de bloco); um processo por run.
run() {
    local tag=$1 mode=$2 N=$3 L=$4 seed=$5 b=$6 file=$7 ref=${8:-}
    local log="$OUT/${tag}.log"
    echo "tag=$tag mode=$mode N=$N L=$L seed=$seed B=$b ref=${ref:--} file=$file start=$(date -Is)" \
        >>"$OUT/runs.out"
    OMP_NUM_THREADS=$THREADS OMP_WAIT_POLICY=PASSIVE \
        "$BIN" "$mode" "$N" "$L" "$K" "$seed" "$b" "$N" "$ref" "real:$file" \
        >"$log" 2>&1
    echo "tag=$tag rc=$? end=$(date -Is)" | tee -a "$OUT/runs.out"
    return 0
}

# ck <tag> -> CK_FINAL do log (ou NA)
ck() { sed -n 's/.*CK_FINAL=\(0x[0-9a-f]*\).*/\1/p' "$OUT/$1.log" | tail -1; }

stage_probe() {
    # Contrato de entrada nos N dos pontos (o loader imprime DATA= mesmo em rc=5)
    run veg13_probe  TOUCH 13000 "$LVEG"  0 "$B" "$VEG" pair:probe
    run sun18_probe  TOUCH 18000 "$LSUN"  0 "$B" "$SUN" pair:probe
    run aloi18_probe TOUCH 18000 "$LALOI" 0 "$B" "$RK"  pair:probe
}

stage_veg() {
    # vegfru N=13000 L=40: goldem do legado + A4 (controle) + A5 (2x) + censo.
    run veg13_ref  ref 13000 "$LVEG" 0 0 "$VEG" "$OUT/g_veg13.bin"
    run veg13_A4   A4  13000 "$LVEG" 0 "$B" "$VEG" "$OUT/g_veg13.bin"
    run veg13_A5   A5  13000 "$LVEG" 0 "$B" "$VEG" "$OUT/g_veg13.bin"
    run veg13_A5b  A5  13000 "$LVEG" 0 "$B" "$VEG" "$OUT/g_veg13.bin"
    run veg13_census A5CENSUS 13000 "$LVEG" 0 "$B" "$VEG"
}

stage_sun() {
    # sun397 N=18000 L=25: o segundo dataset da motivacao da tese.
    run sun18_ref  ref 18000 "$LSUN" 0 0 "$SUN" "$OUT/g_sun18.bin"
    run sun18_A4   A4  18000 "$LSUN" 0 "$B" "$SUN" "$OUT/g_sun18.bin"
    run sun18_A5   A5  18000 "$LSUN" 0 "$B" "$SUN" "$OUT/g_sun18.bin"
    run sun18_A5b  A5  18000 "$LSUN" 0 "$B" "$SUN" "$OUT/g_sun18.bin"
    run sun18_census A5CENSUS 18000 "$LSUN" 0 "$B" "$SUN"
}

stage_aloi() {
    # ALOI N=18000 L=1150: referencia cruzada do protocolo da Etapa 1.
    run aloi18_ref ref 18000 "$LALOI" 0 0 "$RK" "$OUT/g_aloi18.bin"
    run aloi18_A5  A5  18000 "$LALOI" 0 "$B" "$RK" "$OUT/g_aloi18.bin"
    run aloi18_census A5CENSUS 18000 "$LALOI" 0 "$B" "$RK"
}

report() {
    # Tabela derivada dos brutos (nada digitado a mao).
    printf '%-16s %-9s %-6s %-20s %s\n' tag mode rc CK_FINAL bitfinal/POS
    local t m rc ckf bit
    while read -r t m; do
        [ -r "$OUT/$t.log" ] || continue
        rc=$(sed -n 's/^tag='"$t"' rc=\([0-9]*\) .*/\1/p' "$OUT/runs.out" | tail -1)
        ckf=$(ck "$t")
        bit=$(grep -o 'BITFINAL=[A-Z]* POS_EQUAL=[0-9.]*' "$OUT/$t.log" | tail -1)
        printf '%-16s %-9s %-6s %-20s %s\n' "$t" "$m" "${rc:-?}" "${ckf:-NA}" "${bit:-NA}"
    done < <(sed -n 's/^tag=\([^ ]*\) mode=\([^ ]*\) .*/\1 \2/p' "$OUT/runs.out")
}

case "$STAGE" in
    probe) stage_probe ;;
    veg)   stage_veg ;;
    sun)   stage_sun ;;
    aloi)  stage_aloi ;;
    all)   stage_veg; stage_sun; stage_aloi ;;
    report) report ;;
    *) echo "estagio desconhecido: $STAGE (probe|veg|sun|aloi|all)"; exit 2 ;;
esac
echo "A5_STAGE_${STAGE}_DONE"