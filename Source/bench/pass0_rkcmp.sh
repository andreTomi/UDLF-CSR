#!/usr/bin/env bash
# pass0_rkcmp.sh -- Passo 0: numerica do MOTOR (pipelines reais LHRR/CPRR sobre
# ALOI N=72000 L=400) sob as DUAS strings de flags -- PROD (Makefile) vs NFM
# (produto sem as 3 flags de fast-math + -ffp-contract=off) -- comparado
# bit-a-bit. Mesmo protocolo de _p2_rkcmp.sh (taskset, PHASE/OMP_THREADS,
# PHASE_RK_OUT + PHASE_DUMP_DIR, REPS=1), so que os dois bracos sao PROD vs
# NFM em vez de pre vs new.
#
# Pergunta: as flags de producao mudam a saida do motor? Se SIM, o registro do
# A4 em Source/src para (o metodo so e confiavel num regime bit-exato contra o
# que foi validado); se NAO (bit-identico), o produto adota o regime NFM e o
# registro segue. Esta e a unica decisao que este script toma -- sem
# investigacao de causa aqui, so o veredito.
#
# Gates (PROD vs NFM no MESMO par pipeline x T): tamanho do .rk, cmp byte a
# byte dos .rk e dos dumps canonicos. rk_cmp.py/dump_cmp.py rodam como INFO
# (classificam a diferenca se houver; nao constroem o gate). Cross-T = info.
# Evidencia: logs em Data/phase_profile/numerics_prod/pass0_*; .rk/dumps
# (920 MB) no cache, fora do repo.
#
# Uso: bash Source/bench/pass0_rkcmp.sh [build|runs|gates|all]
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1
CXX=${CXX:-g++}
OUT=${OUT:-Data/phase_profile/numerics_prod}
CACHE=${CACHE:-/home/andre/udlf_cache/pass0}
IN=${IN:-/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin}
N=${N:-72000}; L=${L:-400}; K=${K:-10}; LIMIT=${LIMIT:-400}; REPS=${REPS:-1}
STAGE=${1:-all}

fail() { echo "PASS0 FAILED: $*" >&2; exit 1; }
mkdir -p "$OUT" || fail "mkdir $OUT"
mkdir -p "$CACHE" || fail "mkdir $CACHE"
[ -r "$IN" ] || fail "input ausente: $IN (udlf_rk_aloi_N72000_L400.bin)"

# --- as duas strings de flags (mesma derivacao de prod_numerics.sh) ----------
PROD_FLAGS=$(make -C Source -s show-flags 2>/dev/null \
             | sed -n 's/^Optimization flags: //p' | head -1)
[ -n "$PROD_FLAGS" ] || fail "nao extrai 'Optimization flags:' do Makefile"
case " $PROD_FLAGS " in *" -ffast-math "*) : ;; *) fail "PROD sem -ffast-math" ;; esac
# PRODC: PROD com a CONTRACAO de FMA desligada, resto do fast-math intacto --
# isola "FMA" de "reassociacao" como causa da divergencia do LHRR (M2).
PRODC_FLAGS="$PROD_FLAGS -ffp-contract=off"
NFM_FLAGS=$(printf '%s\n' "$PRODC_FLAGS" \
            | sed -e 's/ -ffast-math / /g' \
                  -e 's/ -funsafe-math-optimizations / /g' \
                  -e 's/ -ffinite-math-only / /g')
[ "$NFM_FLAGS" != "$PROD_FLAGS" ] || fail "NFM identico a PROD"
case " $NFM_FLAGS " in
  *" -ffast-math "*|*" -funsafe-math-optimizations "*|*" -ffinite-math-only "*)
    fail "NFM ainda contem fast-math" ;;
esac
case " $NFM_FLAGS " in *" -ffp-contract=off "*) : ;; *) fail "NFM sem -ffp-contract=off" ;; esac

flags_of() { case "$1" in PROD) printf '%s' "$PROD_FLAGS" ;; PRODC) printf '%s' "$PRODC_FLAGS" ;; NFM) printf '%s' "$NFM_FLAGS" ;; esac; }

stage_build() {
    local R
    for R in PROD PRODC NFM; do
        echo "build $R"
        # shellcheck disable=SC2086
        $CXX $(flags_of "$R") -I Source/src Source/bench/bench_phase.cpp \
            -o "/tmp/bp_$R" >"$OUT/pass0_build_$R.log" 2>&1 \
            || { tail -20 "$OUT/pass0_build_$R.log"; fail "build $R"; }
    done
}

# run_one <regime> <pipe> <T> <mask> -> logs + .rk + dumps no cache
run_one() {
    local R=$1 pipe=$2 T=$3 mask=$4
    local tag="${pipe}_t${T}_${R}"
    local dd="$CACHE/dump_$tag"
    mkdir -p "$dd" || fail "mkdir $dd"
    echo "run $tag (taskset $mask)"
    taskset -c "$mask" env OMP_NUM_THREADS="$T" PHASE_THREADS="$T" \
        PHASE_RK_OUT="$CACHE/$tag.rk" PHASE_DUMP_DIR="$dd" \
        "/tmp/bp_$R" real "$IN" "$N" "$L" "$K" "$LIMIT" "$pipe" "$REPS" \
        >"$OUT/pass0_$tag.log" 2>&1
    local rc=$?
    echo "tag=$tag rc=$rc regime=$R pipe=$pipe T=$T end=$(date -Is)" \
        >>"$OUT/pass0_runs.out"
    grep -a -m1 '^RKOUT,' "$OUT/pass0_$tag.log" || true
}

stage_runs() {
    local R pipe T mask
    for R in PROD NFM; do
        for pipe in lhrr cprr; do
            for T in 1 12; do
                if [ "$T" = 1 ]; then mask=0; else mask=0-11; fi
                run_one "$R" "$pipe" "$T" "$mask"
            done
        done
    done
}
stage_gates() {
    local pipe T a b base f da db
    local bad=0
    {
        echo "# pass0 gates -- $(date -Is)"
        echo "# PROD: $PROD_FLAGS"
        echo "# PRODC: $PRODC_FLAGS"
        echo "# NFM:  $NFM_FLAGS"
        echo "# input=$IN N=$N L=$L k=$K limit=$LIMIT reps=$REPS"
    } >"$OUT/pass0_flags.txt"
    cat "$OUT/pass0_flags.txt"
    printf 'pipe T  rk_size  rk_cmp  dumps_cmp  verdict\n' | tee "$OUT/pass0_gate.txt"
    for pipe in lhrr cprr; do
        for T in 1 12; do
            a="$CACHE/${pipe}_t${T}_PROD.rk"
            b="$CACHE/${pipe}_t${T}_NFM.rk"
            local size_ok=NO rk=NA dumps=IDENTICAL v=FAIL gbad=0
            if [ ! -r "$a" ] || [ ! -r "$b" ]; then
                echo "$pipe $T MISSING_RK" | tee -a "$OUT/pass0_gate.txt"; bad=1; continue
            fi
            if [ "$(stat -c%s "$a")" -eq $((N * L * 4)) ] && \
               [ "$(stat -c%s "$b")" -eq $((N * L * 4)) ]; then size_ok=YES; else size_ok=NO; gbad=1; fi
            if cmp -s "$a" "$b"; then rk=IDENTICAL; else rk=DIFF; gbad=1; fi
            da="$CACHE/dump_${pipe}_t${T}_PROD"; db="$CACHE/dump_${pipe}_t${T}_NFM"
            for f in "$da"/*.bin; do
                base=$(basename "$f")
                if [ ! -r "$db/$base" ]; then dumps="MISSING:$base"; gbad=1; break; fi
                if ! cmp -s "$f" "$db/$base"; then dumps="DIFF:$base"; gbad=1; break; fi
            done
            if [ "$gbad" = 0 ]; then v=PASS; else v=FAIL; bad=1; fi
            printf '%s %s  %s  %s  %s  %s\n' "$pipe" "$T" "$size_ok" "$rk" "$dumps" "$v" \
                | tee -a "$OUT/pass0_gate.txt"
            {
                echo "--- md5 ($pipe T=$T, PROD NFM) ---"
                md5sum "$a" "$b"
                echo "--- rk_cmp.py info ($pipe T=$T) ---"
            } >>"$OUT/pass0_gate.txt"
            python3 "Source/bench/rk_cmp.py" "$a" "$b" "$N" "$L" >>"$OUT/pass0_gate.txt" 2>&1 || true
            echo "--- dump_cmp.py info ($pipe T=$T) ---" >>"$OUT/pass0_gate.txt"
            echo "(por-arquivo em pass0_report.txt; dump_cmp.py compara arquivos, nao diretorios)" \
                >>"$OUT/pass0_gate.txt"
        done
    done
    echo "--- cross-T (info, nao gate) ---" >>"$OUT/pass0_gate.txt"
    for pipe in lhrr cprr; do
        for R in PROD NFM; do
            if cmp -s "$CACHE/${pipe}_t1_${R}.rk" "$CACHE/${pipe}_t12_${R}.rk"; then
                echo "IDENTICAL ${pipe} t1 == t12 ($R)" >>"$OUT/pass0_gate.txt"
            else
                echo "DIFF ${pipe} t1 != t12 ($R)" >>"$OUT/pass0_gate.txt"
            fi
        done
    done
    if [ "$bad" = 0 ]; then
        echo "PASS0 VERDICT: BIT-IDENTICAL (motor PROD == NFM em lhrr/cprr, T=1/12)" \
            | tee -a "$OUT/pass0_gate.txt"
    else
        echo "PASS0 VERDICT: DIVERGED (ver pass0_gate.txt -- parar aqui)" \
            | tee -a "$OUT/pass0_gate.txt"
    fi
    return "$bad"
}

# stage_report: inventario COMPLETO (todos os dumps, nao so o primeiro DIFF) +
# classificacao por arquivo (dump_cmp.py: structural vs ultimos bits) + md5 do
# input (checagem de vacuidade: um .rk igual ao input tornaria o gate vazio).
stage_report() {
    local pipe T R da db f base
    {
        echo "== pass0 report -- $(date -Is)"
        echo "input md5: $(md5sum "$IN" | cut -d' ' -f1)  ($IN)"
        for pipe in lhrr cprr; do
            for R in PROD NFM; do
                echo "-- ${pipe} $R rk md5 (T=1, T=12) --"
                md5sum "$CACHE/${pipe}_t1_${R}.rk" "$CACHE/${pipe}_t12_${R}.rk"
            done
        done
        for pipe in lhrr cprr; do
            for T in 1 12; do
                da="$CACHE/dump_${pipe}_t${T}_PROD"; db="$CACHE/dump_${pipe}_t${T}_NFM"
                echo "-- ${pipe} T=$T dumps (PROD vs NFM) --"
                for f in "$da"/*.bin; do
                    base=$(basename "$f")
                    if [ ! -r "$db/$base" ]; then echo "  $base MISSING_IN_NFM"; continue; fi
                    if cmp -s "$f" "$db/$base"; then
                        echo "  $base IDENTICAL ($(stat -c%s "$f") B)"
                    else
                        echo "  $base DIFF ($(stat -c%s "$f") B vs $(stat -c%s "$db/$base") B):"
                        python3 "Source/bench/dump_cmp.py" "$f" "$db/$base" 4 2>&1 | sed 's/^/    /'
                    fi
                done
            done
        done
    } >"$OUT/pass0_report.txt" 2>&1
    cat "$OUT/pass0_report.txt"
}

# --- M2: braco PRODC (FMA vs reassociacao) ---------------------------------
stage_m2_runs() {
    run_one PRODC lhrr 1  0
    run_one PRODC lhrr 12 0-11
    run_one PRODC cprr 1  0
    run_one PRODC cprr 12 0-11
}

# m2_pair <T> <pipe> <A> <B>: cmp de rk + inventario de dumps do par (A vs B)
m2_pair() {
    local T=$1 pipe=$2 A=$3 B=$4
    local fa="$CACHE/${pipe}_t${T}_${A}.rk" fb="$CACHE/${pipe}_t${T}_${B}.rk"
    local da="$CACHE/dump_${pipe}_t${T}_${A}" db="$CACHE/dump_${pipe}_t${T}_${B}"
    echo "-- ${pipe} T=$T: $A vs $B --"
    if [ ! -r "$fa" ] || [ ! -r "$fb" ]; then echo "  MISSING_RK"; return 1; fi
    if cmp -s "$fa" "$fb"; then echo "  rk IDENTICAL (md5 $(md5sum "$fa" | cut -d' ' -f1))"; else
        echo "  rk DIFF: $(md5sum "$fa" | cut -d' ' -f1) vs $(md5sum "$fb" | cut -d' ' -f1)"
        python3 "Source/bench/rk_cmp.py" "$fa" "$fb" "$N" "$L" 2>&1 | sed 's/^/    /'
    fi
    local base
    for f in "$da"/*.bin; do
        base=$(basename "$f")
        [ -r "$db/$base" ] || { echo "  $base MISSING_IN_$B"; continue; }
        if cmp -s "$f" "$db/$base"; then echo "  $base IDENTICAL"; else
            echo "  $base DIFF:"
            python3 "Source/bench/dump_cmp.py" "$f" "$db/$base" 2 2>&1 | sed 's/^/    /'
        fi
    done
}

# Veredito PRE-REGISTRADO (escrito antes de rodar, para nao decidir a posteriori):
#   PRODC == NFM  -> a contracao de FMA e a causa; -ffp-contract=off (mantendo
#                    fast-math) ja estabiliza a ESTRUTURA
#   PRODC == PROD -> a causa e a reassociacao (so NFM resolve)
#   nenhum dos dois -> causa mista (registrar)
stage_m2_report() {
    local T lhrr_cn lhrr_cp cprr_cp
    {
        echo "== pass0 M2 report (FMA vs reassociacao) -- $(date -Is)"
        echo "# PRODC: $PRODC_FLAGS"
        for T in 1 12; do
            m2_pair "$T" lhrr PRODC NFM;  lhrr_cn=$?
            m2_pair "$T" lhrr PRODC PROD; lhrr_cp=$?
            m2_pair "$T" cprr PRODC PROD; cprr_cp=$?
        done
        echo
        echo "== veredito pre-registrado =="
        local a="$CACHE/lhrr_t1_PRODC.rk" b="$CACHE/lhrr_t1_NFM.rk" c="$CACHE/lhrr_t1_PROD.rk"
        if cmp -s "$a" "$b" && cmp -s "$a" "$c"; then
            echo "M2: PRODC == NFM == PROD (nenhuma das duas explica; revisar)"
        elif cmp -s "$a" "$b"; then
            echo "M2 VERDICT: PRODC == NFM != PROD -> a CONTRACAO DE FMA e a causa"
            echo "  (=> -ffp-contract=off mantendo o fast-math estabiliza o LHRR)"
        elif cmp -s "$a" "$c"; then
            echo "M2 VERDICT: PRODC == PROD != NFM -> a REASSOCIACAO e a causa"
            echo "  (=> so remover as 3 flags de fast-math resolve)"
        else
            echo "M2 VERDICT: causa MISTA (PRODC difere de PROD e de NFM)"
        fi
    } >"$OUT/pass0_m2.txt" 2>&1
    cat "$OUT/pass0_m2.txt"
}

# --- M1: a perturbacao mexe em P@k/MAP? (mesmo pipeline, via driver) -------
# Replica a semantica do avaliador publicado (pk_eval.py) sobre os .rk dos dois
# bracos do LHRR. AVISO declarado: isto e o pipeline do driver (ALOI 72k, L=400,
# k=10, limit=400, T=1) -- proxy indicativo, NAO equivalente a config publicada
# do framework em Data/Unified_Accuracy_Results.csv (essa fica como M4, pesada).
stage_quality() {
    local R rk out
    : >"$OUT/pass0_quality.txt"
    for R in PROD PRODC NFM; do
        rk="$CACHE/lhrr_t1_$R.rk"
        [ -r "$rk" ] || { echo "$R: .rk ausente ($rk)" >>"$OUT/pass0_quality.txt"; continue; }
        out="$OUT/pass0_pk_$R.txt"
        python3 Source/bench/pk_eval.py eval --rk "$rk" --bin \
            --classes Datasets/aloi/classes.txt --L "$L" \
            --ks 4,5,10,15,20 --map-ks 100,400 \
            >"$out" 2>&1 || echo "# pk_eval rc!=0 ($R)" >>"$out"
        {
            echo "== $R ($rk) =="
            grep -v '^$' "$out"
        } >>"$OUT/pass0_quality.txt"
    done
    {
        echo
        echo "== publicado (Data/Unified_Accuracy_Results.csv, LHRR/aloi) -- referencia externa =="
        head -1 Data/Unified_Accuracy_Results.csv
        grep -m2 -e '^aloi,LHRR' Data/Unified_Accuracy_Results.csv || true
    } >>"$OUT/pass0_quality.txt"
    cat "$OUT/pass0_quality.txt"
}

case "$STAGE" in
    build) stage_build ;;
    runs)  stage_runs ;;
    gates) stage_gates ;;
    report) stage_report ;;
    m2_runs) stage_m2_runs ;;
    m2_report) stage_m2_report ;;
    quality) stage_quality ;;
    all)   stage_build; stage_runs; stage_m2_runs; stage_gates; stage_m2_report; stage_quality; stage_report ;;
    *) echo "estagio desconhecido: $STAGE (build|runs|m2_runs|gates|m2_report|quality|report|all)"; exit 2 ;;
esac
echo "PASS0_STAGE_${STAGE}_DONE"