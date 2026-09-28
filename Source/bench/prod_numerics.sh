#!/usr/bin/env bash
# prod_numerics.sh -- auditoria da numerica do motor sob as flags EXATAS do build
# do produto, com digital de VALORES (nao de lista de casos) e custo de tempo.
#
# As tres perguntas, e o estagio que responde cada uma:
#   Q1 os NUMEROS do motor mudam quando ele sai das flags da malha (-O2) e entra
#      nas do produto (-O3 -march=native -mfma -ffast-math -DNDEBUG ...)?
#         -> estagios `suites` (md5) e `values` (nnz + checksum)
#   Q2 o pin bit-a-bit do port BFSTree sobrevive a essas flags, e o que exatamente
#      dentro delas o quebra?  -> estagio `golden` nos quatro regimes
#   Q3 quanto custa, em tempo, o regime que preserva os bits? -> estagio `timing`
#
# O que JA EXISTIA e o que isto acrescenta (nao vender re-medicao como achado):
# o cabecalho de Source/tests/gate_bfstree.sh ja registrava que "as flags do build
# do produto" movem o CK_FINAL do A4 em N=800 (0x7b70dfb3.. -> 0x0423b127..) e ate
# o golden em N=4000. Aquilo foi medido com uma string de flags PARAFRASEADA no
# comentario, sem a string real do Makefile, sem atribuicao entre as partes do
# -ffast-math, sem digital de valores do motor e sem custo de tempo. Este script
# mede as quatro coisas: a string vem de `make -C Source -s show-flags`, o regime
# PRODC isola a contracao de FMA do resto do -ffast-math, `values` grava
# nnz/checksum, e `timing` cobra o preco.
#
# Por que as suites de testes NAO respondiam Q1 sozinhas: os md5 gravados em
# Data/phase_profile/etapa_b/s2_close (b6a4f54e.., 112fb487..) foram produzidos
# com as flags de Source/tests/run_all.sh (-std=gnu++2b -O2 -fopenmp) e variavam
# -DNDEBUG / -DPHASE_PROFILE, nao flags de otimizacao. Comparar regime de build
# com eles exige reconstruir exatamente aquelas flags -- e o que o regime BENCH
# faz, de proposito: se BENCH nao reproduzir o prefixo registrado, a auditoria
# inteira esta medindo outra coisa, e o script diz isso em vez de calar.
#
# Limitacoes declaradas:
#  - o checksum do bench_e2e passa por uma soma serial em double dos valores do
#    resultado (Source/bench/bench_e2e.cpp:145), ou seja, pelo MESMO instrumento
#    que o -ffast-math reassocia: divergencia APENAS no checksum deve ser lida
#    com isso em mente. nnz e inteiro e imune ao instrumento -- e a metade dura do
#    digital. Resolucao do checksum: 1e-6 em valor absoluto.
#  - as suites comparam com igualdade exata; se uma flag mudar um valor sem
#    violar igualdade, o md5 delas nao percebe. Nao e defeito daqui: e a
#    diferenca entre alarme de assercao e digital de valores.
#  - -march=native amarra o resultado a esta maquina; "PROD" so e reproduzivel no
#    mesmo CPU (gravamos model name em flags.out por isso).
#  - o golden do port e sintetico em N=800 (debito 6.4 do dossie): bit-exatidao
#    em dados reais grandes continua em aberto depois desta auditoria.
#
# Uso: bash Source/bench/prod_numerics.sh [estagio ...]
#   flags | suites | values | golden | timing | verdict | all
# Variaveis: THREADS REPS_SYNTH REPS_REAL REAL_N REAL_L RK OUT CXX
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-Data/phase_profile/numerics_prod}
THREADS=${THREADS:-12}
REPS_SYNTH=${REPS_SYNTH:-3}
REPS_REAL=${REPS_REAL:-2}
RK=${RK:-Datasets/aloi/rkLists.txt}
# Caso real de forma de producao que PASSA no contrato do carregador: em N=18000
# o sub-corpus deixa min_survivors=1171, entao L=1150 (com L=1400 a Etapa 1 levou
# rc=5 -- ver Data/phase_profile/bfstree_step1/runs.out).
REAL_N=${REAL_N:-18000}
REAL_L=${REAL_L:-1150}
BIN=${BIN:-/tmp/numprod}
mkdir -p "$BIN" || exit 1
mkdir -p "$OUT" || exit 1
STAGES=("$@"); [ "${#STAGES[@]}" -gt 0 ] || STAGES=(all)

fail() { echo "NUMERICS FAILED: $*" >&2; exit 1; }

# ---------------------------------------------------------------- regimes --
# PROD vem do Makefile. Nada aqui e redigitado: se o Makefile mudar, a auditoria
# muda junto -- que e o ponto.
PROD_FLAGS=$(make -C Source -s show-flags 2>/dev/null \
             | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD_FLAGS" ] || fail "nao consegui extrair 'Optimization flags:' do Makefile"
case " $PROD_FLAGS " in
  *" -ffast-math "*) : ;;
  *) fail "PROD sem -ffast-math -- extracao falhou, a auditoria sairia vazia" ;;
esac
# PRODC: so a CONTRACAO de FMA desligada, resto do -ffast-math intacto. Serve para
# atribuir: se PRODC ja conserta o pin, o culpado e o FMA, nao o -ffast-math
# inteiro -- e a diferenca entre "mudou alguma coisa" e "mudou ISTO".
PRODC_FLAGS="$PROD_FLAGS -ffp-contract=off"
# NFM: o candidato da decisao 1 do dossie 7.4 -- otimizacao de virgula flutuante
# agressiva fora, resto do produto (e o -DNDEBUG) dentro.
NFM_FLAGS=$(printf '%s\n' "$PROD_FLAGS" \
           | sed -e 's/ -ffast-math / /g' \
                 -e 's/ -funsafe-math-optimizations / /g' \
                 -e 's/ -ffinite-math-only / /g')
NFM_FLAGS="$NFM_FLAGS -ffp-contract=off"
# BENCH: as flags da malha literalmente como Source/tests/run_all.sh usa, para o
# md5 ser comparavel com o que esta registrado no repo.
BENCH_FLAGS="-std=gnu++2b -O2 -fopenmp"

# Guardas: a derivacao tem que TER acontecido. Sem isto a auditoria "passaria" por
# vacuidade -- os quatro regimes poderiam ser o mesmo binario.
[ "$NFM_FLAGS" != "$PROD_FLAGS" ] || fail "NFM identico a PROD: a substituicao nao ocorreu"
case " $NFM_FLAGS " in
  *" -ffast-math "*|*" -funsafe-math-optimizations "*|*" -ffinite-math-only "*)
      fail "NFM ainda contem uma flag de fast-math" ;;
esac
case " $NFM_FLAGS " in *" -ffp-contract=off "*) : ;; *) fail "NFM sem -ffp-contract=off" ;; esac
case " $BENCH_FLAGS " in *" -O2 "*) : ;; *) fail "BENCH sem -O2" ;; esac

flags_of() {
    case "$1" in
        PROD)  echo "$PROD_FLAGS" ;;
        PRODC) echo "$PRODC_FLAGS" ;;
        NFM)   echo "$NFM_FLAGS" ;;
        BENCH) echo "$BENCH_FLAGS" ;;
        *) return 1 ;;
    esac
}
# REGIMES/TIME_REGIMES aceitam override pela ambiente (espacos separam os bracos)
# -- usado para smoke test de um so braco sem reescrever o script.
read -r -a REGIMES <<<"${REGIMES:-PROD PRODC NFM BENCH}"
# Regimes de tempo: PRODC fica de fora de proposito -- ele so existe para atribuir
# a causa da divergencia, nao e candidato de build.
read -r -a TIME_REGIMES <<<"${TIME_REGIMES:-PROD NFM BENCH}"

build_one() {  # build_one <regime> <src> <bin> ; log de build em $OUT
    local R=$1 src=$2 out=$3 fl
    fl=$(flags_of "$R") || fail "regime desconhecido: $R"
    echo "  [$R] $(basename "$src")"
    # shellcheck disable=SC2086
    $CXX $fl -I Source/src "$src" -o "$out" \
        >"$OUT/build_$(basename "$out")_$R.log" 2>&1
}

run_env() { OMP_NUM_THREADS=$THREADS OMP_WAIT_POLICY=PASSIVE "$@"; }

# Prefixos de md5 registrados no repo (Data/phase_profile/etapa_b/s2_close:84,
# gravados la truncados a 8 hex -- por isso so o prefixo e comparavel).
REC_MD5_test_csr_ops=b6a4f54e
REC_MD5_test_multiply_and_prune=112fb487

stage_flags() {
    echo "== [1/6] regimes =="
    {
        echo "# extraido em $(date -Is) por Source/bench/prod_numerics.sh"
        echo "host=$(uname -srm)"
        echo "compiler=$($CXX --version | head -1)"
        sed -n 's/^model name[[:space:]]*:[[:space:]]*/cpu_model=/p' /proc/cpuinfo | head -1
        echo
        echo "== bruto: make -C Source -s show-flags =="
        make -C Source -s show-flags 2>&1
        echo
        for R in "${REGIMES[@]}"; do
            echo "== regime $R =="
            echo "flags: $(flags_of "$R")"
        done
        echo
        echo "== guardas =="
        echo "PROD contem -ffast-math: sim (sem isso o script teria abortado)"
        echo "PROD contem -DNDEBUG: $(case " $PROD_FLAGS " in *" -DNDEBUG "*) echo sim ;; *) echo NAO ;; esac)  # as assert exatas do codigo moram aqui"
        echo "NFM sem as tres flags de fast-math: verificado no preambulo"
        echo "BENCH = flags de Source/tests/run_all.sh: '$BENCH_FLAGS'"
    } >"$OUT/flags.out" 2>&1 || fail "stage_flags"
    cat "$OUT/flags.out"
}

stage_suites() {
    echo "== [2/6] suites de igualdade exata nos quatro regimes =="
    : >"$OUT/suites.out"
    local t R bin log rc md5 res rec
    for t in test_csr_ops test_multiply_and_prune; do
        for R in "${REGIMES[@]}"; do
            bin="$BIN/${t}_$R"
            log="$OUT/suite_${t}_$R.log"
            if ! build_one "$R" "Source/tests/$t.cpp" "$bin"; then
                echo "$t $R BUILD_FAIL ver build_${t}_$R.log" | tee -a "$OUT/suites.out"
                continue
            fi
            run_env "$bin" >"$log" 2>&1; rc=$?
            # Normalizacao identica a run_all.sh: a 1a linha imprime a contagem de
            # threads, e a linha "Build" muda com a string de flags.
            md5=$(tail -n +2 <"$log" | grep -v Build | md5sum | cut -d' ' -f1)
            res=$(tail -1 "$log" | tr -d '\r')
            rec=$(eval echo "\$REC_MD5_$t")
            # O case fica em instrucao propria: `case ... esac` dentro de `$( )`
            # dentro de aspas duplas BASH ROUPA o padrao (o bash fecha a
            # substituicao no primeiro `)` e o resto vira comando solto -- medido,
            # nao deduzido; a mesma armadilha quebrava os marcadores do golden).
            pref=DIFERENTE; case $md5 in "$rec"*) pref=IGUAL ;; esac
            echo "$t $R rc=$rc md5=$md5 registrado=$rec prefixo=$pref ultima_linha='$res'" \
                >>"$OUT/suites.out"
        done
    done
    local t uniq
    for t in test_csr_ops test_multiply_and_prune; do
        uniq=$(sed -n "s/^$t [A-Z]* rc=0 md5=\([0-9a-f]*\).*/\1/p" "$OUT/suites.out" \
               | sort -u | wc -l)
        echo "$t MD5_UNICO_ENTRE_REGIMES=$uniq" >>"$OUT/suites.out"
    done
    cat "$OUT/suites.out"
}

stage_values() {
    echo "== [3/6] digital de VALORES do motor (bench_e2e: nnz + checksum) =="
    # Duas palavras por run, e nao e a toa: nnz e inteiro e nao depende do
    # instrumento de medicao -- se ele mudar, a mudanca e visivel ao produto (sao
    # entradas a mais/menos na matriz, nao ULPs). checksum e o valor agregado, e
    # passa pela soma serial em double de bench_e2e.cpp:145, isto e, pelo mesmo
    # instrumento que o -ffast-math reassocia.
    : >"$OUT/values.digest"
    local R v N log rc line
    for R in "${REGIMES[@]}"; do
        if ! build_one "$R" Source/bench/bench_e2e.cpp "$BIN/bench_e2e_$R"; then
            echo "$R build build build build build" >>"$OUT/values.digest"
            echo "  [$R] BUILD_FAIL de bench_e2e" >&2
            continue
        fi
        for v in dense adaptive; do
            for N in 10000 100000 1000000; do
                log="$OUT/e2e_${v}_${N}_$R.log"
                run_env "$BIN/bench_e2e_$R" "$v" "$N" >"$log" 2>&1; rc=$?
                line=$(sed -n 's/.*nnz=[ ]*\([0-9]*\)[ ]*checksum=[ ]*\([-0-9.]*\).*/\1 \2/p' \
                       "$log" | tail -1)
                # cols: regime variante N rc nnz checksum
                echo "$R $v $N $rc ${line:-nenhum nenhum}" >>"$OUT/values.digest"
            done
        done
    done
    awk 'NF>=6 {k=$2"/"$3; nnz[k]=nnz[k]" "$1"="$5; cks[k]=cks[k]" "$1"="$6;
                u[k"|nnz|"$5]=1; u[k"|ck|"$6]=1}
         END{for (x in u){split(x,p,"|"); c[p[1]" "p[2]]++}
             for (k in nnz) printf "%-16s nnz_distinto=%d checksum_distinto=%d  nnz:%s  ck:%s\n", \
                    k, c[k" nnz"], c[k" ck"], nnz[k], cks[k]}' \
        "$OUT/values.digest" | sort >"$OUT/values_cmp.txt"
    cat "$OUT/values_cmp.txt"
}

ck_of()  { sed -n 's/.*CK_FINAL=\(0x[0-9a-f]*\).*/\1/p' "$1" | tail -1; }
bit_of() { sed -n 's/.*BITFINAL=\([A-Z]*\) POS_EQUAL=\([0-9.]*\).*/\1 \2/p' "$1" | tail -1; }
# eq <a> <b> -> IGUAL|DIFERENTE. Um `case ... esac` dentro de `$( )` dentro de
# aspas duplas e realcado VAZIO pelo bash -- foi assim que a primeira sonda
# perdeu os tres marcadores pin800_O2/hist_*. Comparacao explícita, sem caso.
eq() { if [ "$1" = "$2" ]; then echo IGUAL; else echo DIFERENTE; fi; }

# gold_case <bin> <regime> <modo> <N> <L> <k> <seed> -- B=256 e limit=N como no
# gate. Os dois fixtures sao os que o repo ja usou, com os parametros O QUE FALTAVAM
# na documentacao (ambos recuperados por medicao: as constantes de bfstree_close
# README 7.2 so existiam na prosa, sem log bruto):
#   N=800  L=300  k=8  seed=42 -- fixture do gate; reproduz 0x7b70dfb313c2b151 /
#          0x0423b12779c26d3d / POS_EQUAL=0.999983 (linha "produto, N=800")
#   N=4000 L=1400 k=20 seed=42 -- fixture da linha "produto, N=4000": reproduz
#          0xa299c0fa9515e5de / 0xed6d982a1d98511e / POS_EQUAL=0.999695
# O golden de cada regime e gravado POR aquele regime: port e legado de regimes
# diferentes nao e a pergunta (o produto compila os dois com a mesma string).
gold_case() {
    local bb=$1 R=$2 m=$3 N=$4 L=$5 k=$6 sd=$7
    run_env "$bb" "$m" "$N" "$L" "$k" "$sd" 256 "$N" "$BIN/golden${N}_$R.bin" \
        >"$OUT/gold_${m}${N}_$R.log" 2>&1
    ck_of "$OUT/gold_${m}${N}_$R.log"
}

stage_golden() {
    echo "== [4/6] pin bit-a-bit do port BFSTree nos quatro regimes =="
    # Pins registrados no repo. Os dois ultimos foram medidos com a string de
    # flags do produto PARAFRASEADA (Source/Makefile:203-206); reproduzi-los ou nao
    # com a string real extraida do Makefile e parte do que este estagio apura.
    local PIN_REF800_O2=0x7b70dfb313c2b151
    local HIST_A4_800_PROD=0x0423b12779c26d3d
    local HIST_REF4000_PROD=0xa299c0fa9515e5de
    : >"$OUT/golden.out"
    local R bb bit ck_ref ck_A ck_A2 ck_A4 ck_ref4000 ck_A4_4000 gate_rc gate_msg d
    for R in "${REGIMES[@]}"; do
        bb="$BIN/bench_bfstree_$R"
        if ! build_one "$R" Source/bench/bench_bfstree.cpp "$bb"; then
            echo "$R BUILD_FAIL" >>"$OUT/golden.out"; continue
        fi
        ck_ref=$(gold_case "$bb" "$R" ref 800 300 8 42)
        ck_A=$(gold_case "$bb" "$R" A 800 300 8 42)
        ck_A2=$(gold_case "$bb" "$R" A2 800 300 8 42)
        ck_A4=$(gold_case "$bb" "$R" A4 800 300 8 42)
        bit=$(bit_of "$OUT/gold_A4800_$R.log")
        # N=4000 e o ponto onde a documentacao diz que "ate o golden se move" --
        # fixture L=1400 k=20 seed=42, a da linha 7.2 (parametros recuperados por
        # medicao, ver gold_case). Medem-se os dois lados no mesmo regime.
        ck_ref4000=$(gold_case "$bb" "$R" ref 4000 1400 20 42)
        ck_A4_4000=$(gold_case "$bb" "$R" A4 4000 1400 20 42)
        # O gate do repo, com as flags do regime sobrepostas (mesma fixture, mesmo
        # pin, mesma assert de contrato): a pergunta e "o make test-bfstree
        # passaria se o produto fosse compilado assim?"
        FLAGS="$(flags_of "$R")" HYP="$BIN/gate_hyp_$R" BIN="$BIN/gate_bench_$R" \
            bash Source/tests/gate_bfstree.sh >"$OUT/gate_$R.log" 2>&1
        gate_rc=$?
        gate_msg=$(sed -n 's/^BFSTREE GATE FAILED: //p' "$OUT/gate_$R.log" | head -1)
        [ -n "$gate_msg" ] || gate_msg=$(tail -1 "$OUT/gate_$R.log")
        echo "$R ck_ref800=$ck_ref ck_A=$ck_A ck_A2=$ck_A2 ck_A4=$ck_A4 bitfinal_A4=$bit \
pin800_O2=$(eq "$ck_ref" "$PIN_REF800_O2") hist_A4_800=$(eq "$ck_A4" "$HIST_A4_800_PROD") \
ck_ref4000=$ck_ref4000 ck_A4_4000=$ck_A4_4000 \
hist_ref4000=$(eq "$ck_ref4000" "$HIST_REF4000_PROD") \
gate_rc=$gate_rc gate_msg='$gate_msg'" >>"$OUT/golden.out"
    done
    # Determinismo intra-regime: o mesmo binario A4, duas execucoes, mesmo
    # CK_FINAL. Sem isto, "a flag mudou o valor" e indistinguivel de "o run e
    # instavel". Verificado de FORA (o script compara), nao por assert interna --
    # assert interna e justamente o que o -DNDEBUG do produto remove.
    for R in "${REGIMES[@]}"; do
        bb="$BIN/bench_bfstree_$R"; [ -x "$bb" ] || continue
        d=$(for i in 1 2; do run_env "$bb" A4 800 300 8 42 256 800 "$BIN/golden800_$R.bin" 2>/dev/null \
              | grep -E 'CK_W=|BITFINAL=' | md5sum; done | sort -u | wc -l)
        echo "$R A4_repeticao_md5_unico=$d" >>"$OUT/golden.out"
    done
    cat "$OUT/golden.out"
}

median_of() {
    sort -n | awk '{a[NR]=$1} END{ if (!NR) {print "NA"; exit}
        if (NR%2) print a[(NR+1)/2]; else printf "%.2f\n", (a[NR/2]+a[NR/2+1])/2 }'
}

# time_case <tag> <reps> <argv do bench...>
# Os reps sao INTERCALADOS entre regimes (r1: PROD,NFM,BENCH / r2: PROD,NFM,BENCH)
# e nao em blocos: o drift de sessao do WSL2 (~15-20%, licao 1 de s2_close) atinge
# os tres bracos igualmente em vez de infectar um braco inteiro. Nao se descarta
# rep0 porque cada rep e um processo novo e o warm-up do bench e interno ao
# processo -- a regra do rep0 existe para reps dentro do mesmo processo.
time_case() {
    local tag=$1 reps=$2 out rep R tot f5 f7
    shift 2
    for rep in $(seq 1 "$reps"); do
        for R in "${TIME_REGIMES[@]}"; do
            out="$OUT/time_${tag}_${R}_r${rep}.log"
            if [ ! -x "$BIN/bench_bfstree_$R" ]; then
                echo "$tag $R $rep NA NA NA" >>"$OUT/timing.raw"; continue
            fi
            run_env "$BIN/bench_bfstree_$R" "$@" >"$out" 2>&1
            tot=$(sed -n 's/.*T_TOTAL_MS=\([0-9.]*\).*/\1/p' "$out" | tail -1)
            f5=$(sed -n 's/.*T_F5_MS=\([0-9.]*\).*/\1/p' "$out" | tail -1)
            f7=$(sed -n 's/.*T_F7_MS=\([0-9.]*\).*/\1/p' "$out" | tail -1)
            echo "$tag $R $rep ${tot:-NA} ${f5:-NA} ${f7:-NA}" >>"$OUT/timing.raw"
            echo "  $tag $R rep=$rep T_TOTAL_MS=${tot:-NA}"
        done
    done
}

stage_timing() {
    echo "== [5/6] custo de tempo (T_TOTAL_MS = F1..F9, exclusive a leitura do arquivo) =="
    local R tag p v vals med mn mx n
    for R in "${TIME_REGIMES[@]}"; do
        build_one "$R" Source/bench/bench_bfstree.cpp "$BIN/bench_bfstree_$R" \
            || echo "  [$R] BUILD_FAIL: o caso de tempo ficara sem este braco" >&2
    done
    : >"$OUT/timing.raw"
    # Caso sintetico: mesma forma da producao (L~N/12), sem tocar nos 3 GB do
    # rkLists -- isola o caminho de computacao, que e o que a flag afeta.
    time_case synth "$REPS_SYNTH" A4 12000 1000 20 7 256 12000
    # Caso real: ALOI na forma que passa no contrato do carregador (ver REAL_L).
    time_case real "$REPS_REAL" A4 "$REAL_N" "$REAL_L" 20 0 256 "$REAL_N" "" "real:$RK"
    : >"$OUT/timing.summary"
    for tag in synth real; do
        for R in "${TIME_REGIMES[@]}"; do
            vals=$(sed -n "s/^$tag $R [0-9]* \([0-9.]*\) .*/\1/p" "$OUT/timing.raw")
            [ -n "$vals" ] || { echo "$tag $R SEM_DADO" >>"$OUT/timing.summary"; continue; }
            med=$(printf '%s\n' "$vals" | median_of)
            mn=$(printf '%s\n' "$vals" | sort -n | head -1)
            mx=$(printf '%s\n' "$vals" | sort -n | tail -1)
            n=$(printf '%s\n' "$vals" | wc -l)
            echo "$tag $R n=$n mediana=$med min=$mn max=$mx" >>"$OUT/timing.summary"
        done
    done
    for tag in synth real; do
        p=$(sed -n "s/^$tag PROD n=[0-9]* mediana=\([0-9.]*\).*/\1/p" "$OUT/timing.summary")
        [ -n "$p" ] || continue
        for R in "${TIME_REGIMES[@]}"; do
            [ "$R" = PROD ] && continue
            v=$(sed -n "s/^$tag $R n=[0-9]* mediana=\([0-9.]*\).*/\1/p" "$OUT/timing.summary")
            [ -n "$v" ] || continue
            echo "$tag ${R}_vs_PROD=$(awk -v a="$v" -v b="$p" 'BEGIN{printf "%+.1f%%", (a/b-1)*100}')" \
                >>"$OUT/timing.summary"
        done
    done
    cat "$OUT/timing.summary"
}

gk() { sed -n "s/^$1 \(.* \)\?\($2=\)\([^ ]*\).*/\3/p" "$OUT/golden.out" | tail -1; }

stage_verdict() {
    echo "== [6/6] veredito (derivado dos artefatos, nada digitado a mao) =="
    local t R u p v
    {
        echo "# numerica do motor sob as flags do produto -- $(date -Is)"
        echo "# gerado por Source/bench/prod_numerics.sh; cada linha e um sed/awk no"
        echo "# artefato bruto do mesmo diretorio (suites.out, values_cmp.txt,"
        echo "# golden.out, timing.summary)."
        echo
        echo "## Q1 -- os valores do motor mudam?"
        for t in test_csr_ops test_multiply_and_prune; do
            u=$(sed -n "s/^$t MD5_UNICO_ENTRE_REGIMES=\([0-9]*\).*/\1/p" "$OUT/suites.out")
            echo "ENGINE_SUITE_${t}_MD5_DISTINTOS=${u:-NA}  # 1 = nenhuma flag mudou a saida"
            echo "ENGINE_SUITE_${t}_CONFERE_REGISTRO_BENCH=$(sed -n "s/^$t BENCH .*prefixo=\([A-Z]*\).*/\1/p" "$OUT/suites.out")  # IGUAL = bate com o md5 gravado em s2_close"
        done
        if [ -s "$OUT/values_cmp.txt" ]; then
            awk 'match($0,/nnz_distinto=[0-9]+/){a=substr($0,RSTART+13,RLENGTH-13)+0; if(a>m)m=a}
                 match($0,/checksum_distinto=[0-9]+/){b=substr($0,RSTART+18,RLENGTH-18)+0; if(b>n)n=b}
                 END{printf "ENGINE_VALORES_DISTINTOS_MAX_NNZ=%d CHECKSUM=%d\n", m+0, n+0
                     printf "ENGINE_VALUES_NNZ_IGUAL_EM_TODOS_REGIMES=%s\n", (m+0<=1?"SIM":"NAO")
                     printf "ENGINE_VALUES_CHECKSUM_IGUAL_EM_TODOS_REGIMES=%s\n", (n+0<=1?"SIM":"NAO")}' \
                "$OUT/values_cmp.txt"
        fi
        echo
        echo "## Q2 -- o pin bit-a-bit do port sobrevive? (golden gravado pelo proprio legado)"
        for R in "${REGIMES[@]}"; do
            echo "GOLDEN_${R}: $(sed -n "s/^$R .*bitfinal_A4=\([A-Z]*\) \([0-9.]*\).*/BITFINAL_A4=\1 POS_EQUAL=\2/p" "$OUT/golden.out" | tail -1) \
PIN800_O2=$(gk "$R" pin800_O2) HIST_A4_800_PROD=$(gk "$R" hist_A4_800) \
HIST_REF4000_PROD=$(gk "$R" hist_ref4000) gate_rc=$(gk "$R" gate_rc) \
A4_repeticao_md5=$(gk "$R" A4_repeticao_md5_unico)"
        done
        echo
        echo "## Q3 -- quanto custa o regime que preserva bits"
        sed -n 's/^\(synth\|real\) /[caso \1] /p' "$OUT/timing.summary"
        # Deltas derivados AQUI do timing.summary ja gravado: o estagio timing
        # custa ~35 min e o veredito tem que ser reexecutavel sozinho (as linhas
        # *_vs_PROD que o stage_timing grava num run futuro nao sao lidas aqui,
        # para o calculo ser idempotente). Padrao: "tag R n=N mediana=...".
        for t in synth real; do
            p=$(sed -n "s/^$t PROD n=[0-9]* mediana=\([0-9.]*\).*/\1/p" "$OUT/timing.summary")
            [ -n "$p" ] || continue
            for R in "${TIME_REGIMES[@]}"; do
                [ "$R" = PROD ] && continue
                v=$(sed -n "s/^$t $R n=[0-9]* mediana=\([0-9.]*\).*/\1/p" "$OUT/timing.summary")
                [ -n "$v" ] || continue
                echo "$t ${R}_vs_PROD=$(awk -v a="$v" -v b="$p" 'BEGIN{printf "%+.1f%%", (a/b-1)*100}')"
            done
        done
    } >"$OUT/verdict.txt" 2>&1
    cat "$OUT/verdict.txt"
}

for s in "${STAGES[@]}"; do
    case "$s" in
        flags)   stage_flags ;;
        suites)  stage_suites ;;
        values)  stage_values ;;
        golden)  stage_golden ;;
        timing)  stage_timing ;;
        verdict) stage_verdict ;;
        all)     stage_flags; stage_suites; stage_values; stage_golden; stage_timing; stage_verdict ;;
        *) echo "estagio desconhecido: $s (flags|suites|values|golden|timing|verdict|all)"; exit 2 ;;
    esac
done
echo "NUMERICS_STAGE_${STAGES[*]}_DONE"




