#!/usr/bin/env bash
# gate_bfstree.sh -- gates do port BFSTree (path CSR A2/A4) que a malha precisa
# conhecer. Roda em ~13 s (os dois builds dominam) e nao toca em Source/src: e o
# fechamento da campanha de medicao, nao a inclusao no produto (debito 6.3 do
# dossie).
#
# Os tres gates, na ordem do arco do RDPAC:
#   [1/3] hipotese F5=SpGEMM (test_bfstree_hyp.cpp): T1/T3 exigem igualdade de
#         bits celula a celula; T4a/T4b em tolerancia (o legado intercala os 3
#         blocos por query, os produtos somam por bloco -- ULPs diferentes sao
#         esperados, mismatch tem que ser 0).
#   [2/3] contrato de entrada do carregador de listas reais: o carregador
#         RECUSA em vez de preencher com lixo. rc=5 se alguma linha tem < L
#         sobreviventes depois de filtrar id<N (foi o que salvou oito runs em
#         N=13k/18k reais), rc=4 se alguma linha nao contem o proprio id.
#   [3/3] bit-exactness do port contra o golden gravado pelo proprio legado
#         (mode=ref), em escala pequena e sintetico: ref/A/A2/A4 tem que
#         fechar com CK_FINAL identico e BITFINAL=YES POS_EQUAL=1.000000.
#
# Postura de build (medida aqui, nao herdada do RDPAC): os dois builds rodam
# -O2 -ffp-contract=off. A flag e DEFENSIVA, nao decisiva: em -O2 ela nao muda
# nada nestes dados (CK_FINAL identico com e sem ela em N=800 e N=4000). O que
# muda o resultado sao as flags do build do produto (Source/Makefile: -O3
# -march=native -mfma -ffast-math): em N=800 o golden do legado continua
# 0x7b70dfb313c2b151 mas o A4 vira 0x0423b12779c26d3d (BITFINAL=NO,
# POS_EQUAL=0,999983); em N=4000 ate o golden se move (0xa299c0fa9515e5de).
# Leitura: o pin abaixo e um contrato -O2, e a identidade bit-a-bit do port NAO
# sobrevive as flags do produto -- o obstaculo a inclusao e a numerica do build,
# nao o port (debito 6.3 / dossie 7.4).
#
# O RSS NAO e pinado aqui: em N=800 o path CSR gasta MAIS que o legado
# (42 MB vs 10 MB medidos) porque o crossover de pico esta em N* ~ 12*L; RSS e
# propriedade de campanha (Data/phase_profile/bfstree_step1), nao de gate.
#
# Build/run ids (medidos nesta maquina, 6 cores / 12 threads):
#   hyp      45 checks, 0 failures; md5 da saida (sem a linha 1, que imprime a
#            contagem de threads) identico em 1/4/12 threads
#   golden   N=800 L=300 k=8 seed=42 B=256 limit=800  -> NNZ_W=553578
#            CK_FINAL=0x7b70dfb313c2b151 em ref/A/A2/A4
#            md5 de CK_W+BITFINAL identico em 1/2/12 threads
#
# Uso: bash Source/tests/gate_bfstree.sh      (de qualquer diretorio)
#      make test-bfstree                       (de Source/)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
HYP=/tmp/gate_bfstree_hyp
BIN=/tmp/gate_bfstree_bench
FLAGS="-std=gnu++2b -O2 -ffp-contract=off -fopenmp"

fail() { echo "BFSTREE GATE FAILED: $*" >&2; exit 1; }

echo "[1/3] hipotese F5 = SpGEMM (T1/T3 bit-exatos, T4a/T4b em tolerancia)"
$CXX $FLAGS -I Source/src Source/tests/test_bfstree_hyp.cpp -o "$HYP" \
  || fail "build do test_bfstree_hyp"
hyp=$("$HYP" | tail -n +2) || fail "execucao do test_bfstree_hyp"
echo "$hyp" | grep -q '^RESULT: 45 checks, 0 failures$' \
  || fail "hyp suite: $(echo "$hyp" | tail -1)"
m=$(for t in 1 4 12; do OMP_NUM_THREADS=$t "$HYP" | tail -n +2 | md5sum; done \
    | sort -u | wc -l)
[ "$m" = 1 ] || fail "hyp nao deterministica entre threads ($m md5)"
echo "      45 checks 0 failures; md5 identico em 1/4/12 threads"

echo "[2/3] contrato de entrada do carregador de listas reais"
$CXX $FLAGS -I Source/src Source/bench/bench_bfstree.cpp -o "$BIN" \
  || fail "build do bench_bfstree"
T=$(mktemp -d) || fail "mktemp"
# 60 linhas, self na posicao 0, L=20 ids < N. ok.txt e a referencia valida;
# short.txt tem a linha 37 com self e só 12 ids; noself.txt tira o 12 da linha 12.
awk 'BEGIN{for(i=0;i<60;i++){printf "%d",i; for(j=1;j<20;j++) printf " %d",(i+j*7)%60; printf "\n"}}' > "$T/ok.txt"
awk 'BEGIN{for(i=0;i<60;i++){if(i==37){printf "37 1 2 3 4 5 6 7 8 9 10 11\n"}else{printf "%d",i; for(j=1;j<20;j++) printf " %d",(i+j*7)%60; printf "\n"}}}' > "$T/short.txt"
awk 'BEGIN{for(i=0;i<60;i++){if(i==12){for(j=0;j<20;j++) printf "%d ",(i+j*11+3)%60; printf "\n"}else{printf "%d",i; for(j=1;j<20;j++) printf " %d",(i+j*7)%60; printf "\n"}}}' > "$T/noself.txt"
for f in ok short noself; do
  [ "$(wc -l < "$T/$f.txt")" = 60 ] || fail "fixture $f tem $(wc -l < "$T/$f.txt") linhas, esperado 60"
done
awk 'BEGIN{for(j=0;j<20;j++) if((12+j*11+3)%60==12) exit 1}' \
  || fail "fixture noself.txt contem o proprio id (bug de fixture)"

rc_of() { local m=$1 f=$2 want=$3; "$BIN" "$m" 60 20 8 7 8 60 "" "real:$T/$f.txt" \
            > "$T/rc_${m}_${f}.out" 2>&1; got=$?
  [ "$got" = "$want" ] || { sed -n '1,3p' "$T/rc_${m}_${f}.out"; fail "mode=$m fixture=$f rc=$got esperado=$want"; }
}
for m in A A4; do
  rc_of "$m" ok     0
  rc_of "$m" short  5
  rc_of "$m" noself 4
done
echo "      rc=0 valida, rc=5 linha curta (<L), rc=4 sem self -- em A e A4"

echo "[3/3] bit-exactness ref/A/A2/A4 vs golden do legado (N=800 L=300 k=8)"
G="$T/golden.bin"
"$BIN" ref 800 300 8 42 256 800 "$G" > "$T/ref.out" 2>&1 \
  || fail "modo ref (golden) nao rodou"
ck_ref=$(grep -o 'CK_FINAL=0x[0-9a-f]*' "$T/ref.out")
[ -n "$ck_ref" ] || fail "golden sem CK_FINAL na saida"
for m in A A2 A4; do
  "$BIN" "$m" 800 300 8 42 256 800 "$G" > "$T/$m.out" 2>&1 \
    || fail "modo $m rc!=0"
  grep -q 'BITFINAL=YES POS_EQUAL=1.000000' "$T/$m.out" \
    || fail "modo $m nao bit-exato: $(grep -o 'BITFINAL=[A-Z]* POS_EQUAL=[0-9.]*' "$T/$m.out")"
  ck=$(grep -o 'CK_FINAL=0x[0-9a-f]*' "$T/$m.out")
  [ "$ck" = "$ck_ref" ] || fail "modo $m $ck != golden $ck_ref"
done
# Pin de arredondamento: este valor so vale com -ffp-contract=off (ver cabecalho).
[ "$ck_ref" = "CK_FINAL=0x7b70dfb313c2b151" ] \
  || fail "golden de arredondamento mudou: $ck_ref (mudou o kernel ou o build?)"
grep -q 'NNZ_W=553578' "$T/ref.out" || fail "NNZ_W do golden mudou"
d=$(for t in 1 2 12; do
      OMP_NUM_THREADS=$t "$BIN" A4 800 300 8 42 256 800 "$G" 2>/dev/null \
        | grep -E 'CK_W=|BITFINAL=' | md5sum
    done | sort -u | wc -l)
[ "$d" = 1 ] || fail "A4 nao deterministica entre threads ($d md5)"
rm -rf "$T"
echo "      $ck_ref NNZ_W=553578 em ref/A/A2/A4; BITFINAL=YES nos tres ports"
echo "BFSTREE GATES: OK"

