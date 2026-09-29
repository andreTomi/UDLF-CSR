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
NFM_FLAGS=$(printf '%s\n' "$PROD_FLAGS" \
            | sed -e 's/ -ffast-math / /g' \
                  -e 's/ -funsafe-math-optimizations / /g' \
                  -e 's/ -ffinite-math-only / /g')
NFM_FLAGS="$NFM_FLAGS -ffp-contract=off"
[ "$NFM_FLAGS" != "$PROD_FLAGS" ] || fail "NFM identico a PROD"
case " $NFM_FLAGS " in
  *" -ffast-math "*|*" -funsafe-math-optimizations "*|*" -ffinite-math-only "*)
    fail "NFM ainda contem fast-math" ;;
esac
case " $NFM_FLAGS " in *" -ffp-contract=off "*) : ;; *) fail "NFM sem -ffp-contract=off" ;; esac

flags_of() { if [ "$1" = PROD ]; then printf '%s' "$PROD_FLAGS"; else printf '%s' "$NFM_FLAGS"; fi; }

stage_build() {
    local R
    for R in PROD NFM; do
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

case "$STAGE" in
    build) stage_build ;;
    runs)  stage_runs ;;
    gates) stage_gates ;;
    report) stage_report ;;
    all)   stage_build; stage_runs; stage_gates; stage_report ;;
    *) echo "estagio desconhecido: $STAGE (build|runs|gates|report|all)"; exit 2 ;;
esac
echo "PASS0_STAGE_${STAGE}_DONE"