# Phase 0 — Instrumentação por fase (`-DPHASE_PROFILE`)

## Dossiês de método neste diretório

O documento de fechamento de um método é o que diz **o que foi provado, com que
build, e o que não é claim**. A régua não está escrita em documento nenhum: está
na malha que o arco do RDPAC deixou (`Source/tests/run_all.sh`, `Source/Makefile`,
`Source/src/Methods/`).

| dossiê | conteúdo | estado |
|---|---|---|
| `bfstree_close/README.md` | encerramento do port BFSTree (Etapas 1→5): portador `A4`, gates, escala, débitos, e §7 sobre inclusão no framework | **fechado** (medição); inclusão no produto = débito 6.3 |
| `bfstree_step1/` | brutos e `aggregate.md` do passo 1: censo estrutural `ρ(W)` em dados reais, crossover de pico, `real_bitex` em N=18k | dados |
| `bfstree_a5/` | **A5 (W por valor)**: implementação no bench + censo A5-vs-denso + validação bit-exata fora do ALOI (vegfru@13k, sun397@18k, ALOI@18k) — 3/3 `BITFINAL=YES`; achados medidos: pico RSS ainda não caiu (F1/F2 densas) e F7 esparso é mais lento em L grande | **fechado** (bench); Trilha C segue bloqueada pelo Passo 0 |
| `numerics_prod/` | auditoria da numérica do build do produto em 4 regimes (PROD/PRODC/NFM/BENCH): suites, valores (nnz/checksum), pin bit-a-bit do port em N=800/4000 + gate, custo de tempo — matéria-prima da §7.2–§7.4 de `bfstree_close` | **fechado** |
| `rho_datasets/` | censo de `ρ(W)` em corel5k/linnaeus/aloi, grade com pares casados, contrato do carregador medido (rc=5 no ALOI N pequeno; rc=4 nunca) e gate `TOUCH == NNZ_W`; **generalização em escala** (vegfru 160k / sun397 108k / cifar10 60k): censo L=1400, gate exato em N=24k e curva `ρ(N)` com barra de erro medida | dados |
| `etapa_a/`, `etapa_b/` | arcos das fases 1–2 do kernel CSR (`multiply_and_prune`), com os scripts `_p2_*`/`_p3_*`/`_fase1_*` da raiz referenciados por `etapa_b/s24/README.md` | histórico |
| RDPAC | **sem dossiê próprio** — a evidência está nos commits `f69e28d`, `79df4b4`, `ca52571`, `b7a8cf7` e nos gates `make test-rdpac` / `run_all.sh` | fechado |

O texto abaixo deste arquivo é o relatório da **Fase 0** (instrumentação por
fase), não um dossiê de método.

## Protocolo e resultados

Protocolo: ≥5 repetições válidas (7 executadas), mediana, primeira descartada,
execuções sequenciais (um processo por vez). Timers = `rdtsc` (contadores por
thread em slots alinhados a 128 B). WSL2 ⇒ sem `perf` de hardware; wall por
`steady_clock` e CPU por `getrusage`. Binários: `-O2 -fopenmp` (sem
`-ffast-math`), idênticos ao protocolo dos testes.

**Overhead da instrumentação** (medianas, 12 threads):
| build | w1 total | w2 total |
|---|---|---|
| sem flag | 108,6 ms | 119,2 ms |
| flag, sample=1 | 122,1 ms (**+12,4%**) | 109,6 ms |
| flag, **sample=8** | **108,2 ms (≈0%)** | **115,8 ms (−2,9%)** |

→ `PHASE_PROFILE_ROW_SAMPLE=8` usado para w1/w2@12t; sample=1 nos demais
(w3 tem regiões por linha longas; rdtsc amortizado, overhead ≪1%).

Equivalência comprovada: md5 do suite de testes idêntico (baseline = sem flag
= com flag = sample 8: `112fb48749110da3c1cf31a5612fbe91`); nnz e checksum do
bench idênticos em todas as builds (w1: 2.500.000 / −503,017268; w2: 2.500.000
/ +277,889357).

## multiply_and_prune — mediana (ms)

F1/F2/MULT_TOTAL = Σ tempo por thread (thread-time); fases de build mostram
wall (makespan). w1/w2@12t: F1/F2 extrapolados de 1-em-8 linhas.

| fase | w1 t1 | w1 t12 | w2 t1 | w2 t12 | w3 t1 | w3 t12 |
|---|---|---|---|---|---|---|
| **total (REP)** | **418,7** | **108,2** | **466,0** | **115,8** | **17024,0** | **4323,8** |
| F1_ACCUM (Σ) | 183,5 | 303,7* | 233,0 | 336,3* | 8905,7 | 27302,8 |
| F2_PRUNE (Σ) | 170,3 | 235,0* | 166,4 | 222,4* | 7364,3 | 15131,8 |
| MULT_TOTAL (wall) | 355,6 | 48,2 | 408,1 | 52,8 | 16272,3 | 3715,7 |
| MULT skew | — | 3,9 | — | 4,2 | — | 160,3 |
| B0 resize temp | 14,0 | 13,8 | 13,8 | 13,6 | 247,2 | 136,5 |
| B1 copy (wall) | 4,8 | 3,5 | 4,8 | 3,5 | 48,6 | 28,1 |
| B2 sort | 26,1 | 26,5 | 25,0 | 26,8 | 279,8 | 275,8 |
| B3 hist (wall) | 5,5 | 1,8 | 5,3 | 3,2 | 36,1 | 10,8 |
| B4 resize vals | 0,6 | 0,7 | 0,6 | 0,7 | 63,7 | 64,8 |
| B5 fill (wall) | 4,4 | 2,8 | 4,5 | 2,8 | 36,7 | 23,5 |
| build (wall Σ) | 55,4 | 49,1 | 54,0 | 50,6 | 712,1 | 539,5 |
| VmHWM final (MB) | 244 | 256 | 244 | 256 | 1710 | 1757 |

*extrapolado (sample=8; direto no s1: F1 336,5 / F2 250,7 — consistente).

## pipe (iteração CPRR: transpose + 2×mult + merge) — 12 threads, ms

| fase | w1 | w3 |
|---|---|---|
| **total (REP)** | **276,7** | **5060,7** |
| TRANSPOSE | 10,3 | 123,7 |
| F1_ACCUM (Σ, 2 mults) | 511,6 | 21225,0 |
| F2_PRUNE (Σ, 2 mults) | 462,7 | 9492,6 |
| MULT_TOTAL (wall) | 47,0 | 1425,6 |
| MERGE (wall) | 8,0 | 131,6 |
| build 4× (wall Σ) | 136,6 | 1770,2 |
| VmHWM final (MB) | 538 | 3434 |

## Leituras principais (insumo para as fases 1–4)

1. **w3 (regime LHRR): acumulação+poda = ~95% do MULT** (F1 52%, F2 43% do
   wall@1t). O `nth_element` com comparador indireto (F2) custa mais que o
   sort global do build ⇒ item 4 (pares contíguos) tem alvo claro.
2. **w1/w2 (regime CPRR): build ≈ 45–50% do wall**; a parte **serial** do
   build (B0+B2+B4 ≈ 41 ms) é quase tão grande quanto o SpGEMM paralelo
   inteiro (MULT wall ≈ 48–53 ms). B2 (sort par_unseq) usa 268–311 ms de CPU
   — é o maior alvo do item 2 (fragmento-CSR por thread). B0 (resize de
   temp_triplets_) gera ~9.800 page faults por chamada.
3. **Skew ≤ 4% do wall** (3,9–4,2 ms em w1/w2@12t; 160 ms em w3@12t com
   bound heterogêneo) ⇒ desbalanceamento existe no w3, mas não domina.
4. **Contenção de memória no acumulador denso**: F1 Σ sobe de 183 ms (1t)
   para ~304–336 ms (12t) — cada thread carrega 8·B.cols ≈ 400–800 KB de
   accumulator+marker e o working-set conjunto estoura LLC.
5. **B2_SORT não escala com OpenMP** (wall ~26 ms em 1t e 12t; TBB interno) —
   e é serial na视角 do motor.
6. **Memória**: pico cresce com o emitted (w3: 20 M triplets → HWM 1,7 GB;
   pipe w3 3,4 GB). HWM before/after do build isola o pico do pipeline
   emit→sort (ex.: w3 t12: 1581→1632 MB no pior build).

Limitações: colunas uniformes (não-kNN); 12 threads em WSL2 (ruído ±5%);
B3 wall cobre só a região do histograma (redução+prefixo contam no total);
F1/F2@12t incluem tempo de stall (rdtsc é wall), não só ciclos de trabalho.

Arquivos: `mult_{w1,w2,w3}_{t1,t12}(.txt|_s8.txt)`, `pipe_{w1,w3}_t12.txt`,
`noflag_{w1,w2}_t12.txt`; agregação: `Source/bench/aggregate_phase.py`.
