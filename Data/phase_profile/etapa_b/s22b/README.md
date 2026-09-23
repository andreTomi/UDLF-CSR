# S2.2 -- A/B legacy vs v2 no pipeline REAL (LHRR + CPRR, ALOI N=72000 L=400)

- Driver unico `/home/andre/udlf_bin/bp_s22` (S2.2a, `8f77887`); a troca de
  builder e **runtime** (`PHASE_BUILDER=legacy|v2`), entao os dois bracos rodam
  **o mesmo binario e os mesmos headers** -- ao contrario do s1_bis (rebuild
  por braco a partir do git).
- Harness: `Source/bench/s22b_run.sh` (resume-avel; skip so se o arquivo
  contiver linha `DONE,`, entao um run morto no meio e refeito).
- Capturas: `t{1,2,12}/raw/` (timing ABBA), `tmem/raw/` (`PHASE_MEM=1`, 6
  builds x 3 reps), `aggregate.txt` (`s22b_agg.py`), `divergence.txt`
  (`dump_cmp` 12 pares), `rk_layer0.txt` (`rk_cmp` 3 pares).

## Protocolo (mesma sessao; politica permanente do S1-bis)

1. warm-up descartado (1 por builder);
2. rep0 de cada bloco descartado (first-touch de processo novo);
3. canario `mult w1` (T=12) antes e depois -> bound de drift da sessao;
4. 3 pairs ABBA: (legacy,v2) (v2,legacy) (legacy,v2) -> drift monotono
   cancela dentro do par;
5. `taskset`: T=1 -> cpu0, T=2 -> 0-1 (par de SMT), T=12 -> 0-11;
   `OMP_NUM_THREADS=PHASE_THREADS=T`;
6. amostras uteis = 3/builder/T (rep1 de p1..p6), mediana. As linhas
   `DRVAGG` agregam TODOS os reps (inclusive rep0), entao a tabela de fases
   usa o bloco `PHASEPROFILE` do rep1 -- `s22b_agg.py` faz isso.

## Tempo (rep1, mediana de 3, ms por rep do LHRR)

| T | legacy | v2 | delta (legacy-v2) | speedup |
|---|--------|----|-------------------|---------|
| 1  | 14882.2 | 7714.0 | +7168,2 ms (+48,2%) | **1,93x** |
| 2  | 8452.5  | 4507.5 | +3945,0 ms (+46,7%) | **1,88x** |
| 12 | 4073.9  | 2153.2 | +1920,7 ms (+47,2%) | **1,89x** |

- As amostras **nao se sobrepoe** em nenhum T (min legacy > max v2).
- Drift do canario: **+7,0% / -6,6% / -11,2%** (T=1/2/12) -- menor que o
  efeito medido (47%) e neutralizado pelo ABBA dentro de cada par.

### Mecanismo (PHASEPROFILE total_ms no rep1; soma entre threads -- em T>1
### dividir por T para ler como wall; razoes legacy/v2 valem pois ambos
### somam do mesmo jeito)

- `BUILD_CALL` legacy/v2 = **2,75x / 2,55x / 2,43x** (T=1/2/12) -- o ganho
  esta no build, nao nas fases de mult/combine (que ficam ~par 1,0x).
- Fases exclusivas do legacy (V1 histograma + sort global) = 0 no v2:
  `B0_TEMP_RESIZE, B1_COPY, B2_SORT, B3_HIST/B3_RED, B5_FILL, BUILD_FINISH`;
  o v2 paga `V4_ROWSORT` (sort por linha) + `V5_COMPACT` (serial, nota de
  contrato devidas no S2.4). Ex.: T=1, `B2_SORT` legacy 8966 ms vs
  `V4_ROWSORT` v2 2419 ms.
- `REPRU`: minflt/rep legacy ~698k vs v2 ~186k -> **3,75x menos faltas de
  pagina** (mecanismo direto do `temp_triplets_` eliminado). A razao
  rep0/rep1 ~ 1,0x **reconfirma a falsificacao** do first-touch (S1-bis).

## Equivalencia: 6 matrizes (dump_cmp v2 vs legacy)

Numeros **identicos em T=1 e T=12** (capturas independentes) -- os dois
builders sao deterministicos inclusive entre thread-counts.

| matriz | estrutura (row_ptr, cols, nnz) | nnz | celulas last-bit | max rel |
|--------|-------------------------------|-----|------------------|---------|
| MM     | identica                      | 40.215.456 | 0 (**bit-identico**) | 0 |
| H      | identica                      | 1.982.199  | 12,2150% (242.126)   | 3,36e-7 |
| S      | identica                      | 7.286.865  | 15,8842% (1.157.459) | 3,99e-7 |
| Srev   | identica                      | 8.685.190  | 13,0978% (1.137.568) | 5,88e-7 |
| Stot   | identica                      | 5.756.659  | 28,0234% (1.613.213) | 6,79e-7 |
| result | identica                      | 5.248.535  | 27,0904% (1.421.850) | 6,96e-7 |

**Nota de relaxamento de aceitacao (decidida no S2.2 aprovado):** o gate de
valores **nao** exige identidade de bits. Celulas afetadas sao somas de >= 3
fragmentos duplicados, ordem que **nenhum dos dois builders especifica**
(`std::sort` instavel; emissao paralela). A aceitacao e, portanto:
(1) estrutura identica (row_ptr + sequencia de colunas + nnz) -- nao
negociavel; (2) valores dentro do bound de ultima casa (**<= 7e-7
relativo**, ~2-4 ulp f32, classe VALUE-ORDER do `dump_cmp.py`); (3)
**igualdade exata na camada 0** (listas rk). Escopo do CPRR no S2.2 =
spot-check layer-0 apenas (plano aprovado), nao a tabela de 6 matrizes.

## Camada 0 (rk_cmp, 72000 x 400 u32)

| comparacao | T | resultado |
|------------|---|-----------|
| LHRR v2 vs legacy | 1  | **BIT-IDENTICAL 72000/72000** |
| LHRR v2 vs legacy | 12 | **BIT-IDENTICAL 72000/72000** |
| CPRR v2 vs legacy (spot-check) | 12 | **BIT-IDENTICAL 72000/72000** |

O mandato de parada ("parar e reportar se a camada 0 divergir em muitas
linhas") **nao foi acionado**. Controle positivo: lhrr vs cprr diverge em
72000/72000 linhas com stats completos (swaps 381/400 em media, tau 0,84) --
prova de que o rk_cmp detecta divergencia quando ela existe.

## Memoria: 6 builds x 2 convencoes (gate: v2 <= legacy nas duas -- PASS 3/3)

| T | conv1 pico DONE (timing) legacy / v2 | conv1 delta | conv2 build-raise MEMBUILD legacy / v2 | conv2 delta | residuo |
|---|--------------------------------------|-------------|----------------------------------------|-------------|---------|
| 1  | 3.358.256 / 1.627.368 kB | +1.730.888 kB (**+51,5%**) | 2.348.224 / 604.224 kB | +1.744.000 kB (**+74,3%**) | **-13.112 kB** |
| 2  | 3.360.632 / 1.630.360 kB | +1.730.272 kB (**+51,5%**) | 2.384.128 / 631.616 kB | +1.752.512 kB (**+73,5%**) | **-22.240 kB** |
| 12 | 3.382.796 / 1.656.172 kB | +1.726.624 kB (**+51,0%**) | 2.317.376 / 653.568 kB | +1.663.808 kB (**+71,8%**) | **+62.816 kB** |

- **conv1** = linha `DONE` das capturas de **timing** (sem `clear_refs` =>
  pico real do processo; mesma convencao do a5bis -- a captura `_mem` e
  reset-afetada e nao serve como pico).
- **conv2** = soma, por rep, de `MEMBUILD delta_hwm_kb` das 6 builds do
  pipeline nas capturas `PHASE_MEM=1` (reset antes de cada build =>
  raise isolado).
- **Sanidade do residuo** (`d(conv1) - d(conv2)`): pequeno (|r| <= 63 MB,
  <= 3,6% do d) e **muda de sinal entre T** (-/-/+). Isso e esperado e nao
  deve ser zerado a forca: conv1 e um maximo sobre o processo inteiro
  (inclui a leitura do .bin, o working set de **emissao dos fragmentos ANTES
  do reset**, teardown/reuso do alocador) e conv2 soma raises medidos cada
  um de uma baseline resetada; as fases nao sao aditivas. Um residuo
  negativo significa que parte da economia de conv1 vem de fora do raise
  interno do build (o `temp_triplets_` de emissao do legacy), nao de um
  erro de medicao.

## Reprodutibilidade

```bash
# build do driver (WSL2, repo em /mnt/c/...)
g++ -std=gnu++2b -O2 -fopenmp -DPHASE_PROFILE -I Source/src \
    Source/bench/bench_phase.cpp -o /home/andre/udlf_bin/bp_s22 -ltbb

# timing ABBA por T (resume-avel) + capturas de memoria
python3 Source/bench/fix_crlf.py Source/bench/s22b_run.sh   # -> /tmp
bash /tmp/s22b_run.sh 1; bash /tmp/s22b_run.sh 2; bash /tmp/s22b_run.sh 12
bash /tmp/s22b_run.sh mem

# dumps + rk de um lado (repetir com PHASE_BUILDER=legacy em diretorio proprio)
PHASE_DUMP_DIR=/home/andre/udlf_cache/s22b_t1/v2 \
PHASE_RK_OUT=/home/andre/udlf_cache/s22b_t1/v2/rk.bin \
taskset -c 0 env OMP_NUM_THREADS=1 PHASE_THREADS=1 \
  /home/andre/udlf_bin/bp_s22 real /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin \
  72000 400 10 400 lhrr 1

python3 Source/bench/s22b_agg.py                    # aggregate.txt
python3 Source/bench/dump_cmp.py <v2>/<M>.bin <legacy>/<M>.bin   # x6, x2 T
python3 Source/bench/rk_cmp.py <v2_rk> <legacy_rk> 72000 400
python3 Source/bench/pk_eval.py gate --dataset aloi # portao do Baseline
```

## Fora de escopo / debitos pos-S2

- GraphBLAS/MKL/bind: recomparacao no ambiente correto (setup de engine 1b
  diferido; este ambiente nao tem as libs).
- Nota de contrato da V5 serial (devida no S2.4) e divida de escala do V1
  histograma (T x rows x 4B) para N/T > ALOI.
- `update_ranked_lists` esta triplificado (LHRR, CPRR no driver, metodos);
  consolidar antes do metodo #3.
- Validade: todas as afirmacoes acima sao de **mesma sessao** por T (ABBA +
  canario). Comparacoes entre sessoes seguem viradas (+25..47%) e nao servem
  de gate -- politica permanente do S1-bis.