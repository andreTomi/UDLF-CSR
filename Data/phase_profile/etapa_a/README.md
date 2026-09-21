# Etapa A — resultados (A0–A6)

Campanha de medição que precede as mudanças no build (Fases 1–6). Objetivo:
substituir o ranking por fase do relatório anterior por **medição**, corrigir os
defeitos de medição encontrados, e fechar as condições de contorno de teste.

- Commit da instrumentação/harness: `2f40980` (testes A6: `9dff5cb`; higiene LF: `e157626`)
- Máquina: **i5-10400F** — `lscpu -e` em `machine_v2.txt`: 6 cores físicos × 2
  SMT = **12 CPUs lógicas**, 1 socket, L3 12 MiB (1 instância), NUMA 1.
  MemTotal 8,08 GB / MemAvailable 5,52 GB. Kernel `6.6.87.2-microsoft-standard-WSL2`.
- Protocolo: mediana com rep 0 descartada, ≥5 reps válidos, execuções sequenciais.
  `rdtsc` (fallback `steady_clock`); wall `steady_clock`; CPU/`minflt`/`majflt` por
  `getrusage`. Timers atrás de `-DPHASE_PROFILE` (sem a flag, release idêntico).
- Arquivos: `aggregate_v2.txt` (autoritativo, sem bind), `aggregate.txt` (campanha
  v1, com bind — ver A1), `raw_v2/` (58 capturas), `mask_audit_v2.log`,
  `noisecheck.txt`, `bindcheck.txt`, `a4_check.txt`, `maskprobe.txt`,
  `probe_bind.txt`, `decisive.txt`, `replication.txt`, `replication/`.

## A0 — Instrumentação (o que passou a ser medido)

Semântica nova, toda inerte sem `-DPHASE_PROFILE`:

| fase | mede |
|---|---|
| `F1_ACCUM` / `F2_PRUNE` | acumulação (`multiply_prune_row`) e poda+emissão (`nth_element`) |
| `B0_TEMP_RESIZE` … `B5_FILL` | build em 6 partes (resize temp serial, copy, **sort**, histograma+prefixo, resize vals serial, fill) |
| `TRANSPOSE`, `MERGE_LOOP`/`COMBINE_LOOP`, `MP_CALL`, `BUILD_CALL`, `ALLOC_SETUP`/`ALLOC_TEARDOWN` | fases de chamada: fecha `REP = Σ(chamadas) + resíduo` |
| `MEM`, `MEMBUILD`, `HWM` | RSS/VmHWM por ponto, ledger por build, pico antes/depois |
| `CTX` | bound medido (média/máx), razão `B.cols/max_bound`, branch escolhida |

Fases seriais medem `user/sys/minflt/majflt` por build; regiões paralelas medem
*skew* (`max − média` do stamp de fim por thread) e `Σ` por thread. Knob
`PHASE_PROFILE_ROW_SAMPLE=N` (amostragem 1-em-N) mantém overhead ≈0 (sample=1:
+12,4% em w1@12t; sample=8: ≈0%).

**Reset de pico**: `echo 5 > /proc/self/clear_refs` antes de cada build
(`CLEAR_REFS_MM_HIWATER_RSS`), o que torna `ΔHWM` *por build* significativo —
`reset_rc=1.0` em todas as linhas de `MEMBUILD`.

## A1 — Curva single-core de verdade, e um defeito de medição grave

### O defeito: `OMP_PROC_BIND=true OMP_PLACES=cores`

O harness da v1 exportava `OMP_PROC_BIND=true OMP_PLACES=cores` em cada run. Nesta
WSL2 isso **não** distribui threads pelo mask: o master é re-pinado ao seu "place"
(core 0 = CPUs 0,1) e o TBB — que roda o `sort` do build e dimensiona a arena pela
afinidade no primeiro uso — colapsa para `max_concurrency=2`.

Medição direta (`bindcheck.txt`, mesma máscara, mesmo nº de threads, 3 reps):

| w1, 12 threads, 12 CPUs | mediana | `max_concurrency` | `B2_SORT` |
|---|---|---|---|
| sem bind | **117,4 ms** | 12 | 24,8 ms |
| com bind | **213,9 ms** | 2 | 68,9 ms |

Efeito confirmado nos brutos da v1: `curve_w1_c0-3_t4`, `curve_w1_c0-5_t6`,
`curve_sib_*` e `curve_w3_call_t12` foram gravados com `affinity=0,1` /
`max_concurrency=2` (`probe_bind.txt` isola a causa; `decisive.txt` mostra que as
máscaras são honradas pelo `taskset`, `maskprobe.txt` que a guarda funciona).
Correção: knob `EA_BIND` no harness, default **0** (sem bind); a campanha v2 é
inteiramente sem bind, com `req_mask`/`affinity_at_entry`/`max_concurrency`
registrados por run e guarda `check_mask` (aborta com `MASKFAIL` se o mask pedido
não coincidir com o nº de CPUs efetivo).

### Curva corrigida (mediana, ms; sem bind, `EA_REPS=6`)

| mask | CPUs | cores físicos | threads | w1 | w2 | w3 |
|---|---|---|---|---|---|---|
| `0` | 1 | 1 (1 SMT) | 1 | 561,7 | 612,0 | 19.153,7 |
| `0-1` | 2 | 1 (2 SMT) | 2 | 321,2 | 307,2 | 9.990,3 |
| `0-3` | 4 | 2 (SMT) | 4 | 203,0 | 197,9 | 5.434,2 |
| `0-5` | 6 | 3 (SMT) | 6 | 131,8 | 152,0 | 4.095,2 |
| `0-11` | 12 | 6 (SMT) | 12 | **110,6** | **108,3** | **3.948,7** |

Speedup w1: 1,75× → 2,77× → 4,26× → **5,08×**; w3: 4,85× na escala completa.
Comparação com a v1 (com bind): 6t 162,8 → **131,8** (−19%), 12t 138,8 → **110,6**
(−20%) — o ganho é exatamente onde o TBB era limitado a 2 threads.

Família *sibling* (1 thread por core, 6 threads) isola o efeito SMT:

| cores físicos | w1 | w2 | w3 |
|---|---|---|---|
| 2 (0,2) | 282,9 | 404,0 | 10.563,0 |
| 3 (0,2,4) | 214,1 | 217,0 | 6.968,1 |
| 4 (…,6) | 215,6 | 188,9 | 5.414,8 |
| 6 (…,8,10) | 133,7 | 155,4 | 4.144,8 |

Também confirma que os pontos de 1 CPU da v1 estavam corretos (o bind é
irrelevante com 1 CPU): `noisecheck.txt` refeito a máquina ociosa dá 514,9 (w1) e
585,1 (w2) ms para `mask=0`, contra 509,7/574,3 na v1 e 561,7/612,0 na v2 — a v2
saiu 4–9% inflada porque a validação do A4 (alocações de 300 MB) rodou em paralelo,
e é essa a margem de ruído da campanha (±5%, WSL2, sem `perf`).

## A2 — Reconciliação do pipe (o resíduo agora é nomeado)

O item anterior ficou entre 27–32% sem atribuição. Fechado por duas razões: as
fases de chamada (`MP_CALL`, `MERGE_CALL`, `COMBINE_CALL`, `BUILD_CALL`,
`TRANSPOSE`, `ALLOC_SETUP/TEARDOWN`) passaram a envolver cada statement do driver,
e a última cauda (`ALLOC_TEARDOWN`) ganhou fase própria. Resultado
(`aggregate_v2.txt`, seção `LEDGER`):

| workload | CPUs | threads | REP (ms) | Σ chamadas (ms) | resíduo | resíduo % |
|---|---|---|---|---|---|---|
| w1 | 0-5 | 6 | 343,1 | 342,9 | +0,2 | **+0,06%** |
| w1 | 0 | 1 | 1.198,4 | 1.204,9 | −6,5 | **−0,54%** |
| w1 | all | 12 | 255,9 | 257,3 | −1,4 | **−0,54%** |
| w3 | 0-5 | 6 | 6.787,9 | 6.893,9 | −105,9 | −1,56% |
| w3 | 0 | 1 | 25.692,1 | 25.608,6 | +83,5 | +0,32% |
| w3 | all | 12 | 4.990,7 | 4.877,4 | +113,3 | **+2,27%** |

Ou seja: **|resíduo| ≤ 2,3%** em todas as configurações. O total do pipe também
caiu com a correção de binding: w1@12t 384,0 → **255,9 ms** (−33%), w3@12t
6.171,0 → **4.990,7 ms** (−19%), w3@6t 7.459,6 → 6.787,9 ms (−9%).

## A3 — Memória: decomposição e custo por triplet

Componentes instrumentados por build: `temp_triplets_` (16 B/entrada emitida —
`Triplet` = row+col+value+is_set), allocators por thread (`capacity_bytes()` e
`blocks_count()` novos no `BlockAllocator`), arrays de contagem, e a saída CSR
(`8 B`/entrada: col+val). `emitted` é contado, então as colunas são *derivadas*,
não estimadas.

`ΔHWM/entrada emitida` (bytes, com `clear_refs=5` antes de cada build):

| workload | 1M emitido | 2M | 5M | 20M | 25M |
|---|---|---|---|---|---|
| w1 @1 thread | 25,4 | 24,9 | 22,3 | 42,1 (pico novo) | 34,1 |
| w1 @12 threads | 27,7 | 26,7 | 22,4 | 42,0 (pico novo) | 34,3 |
| w3 @1 thread | 25,0 | 24,7 | — | 34,6 | 24,7 (pico novo) |
| **aloi LHRR (57,6M)** | — | — | — | — | **35,3** (ΔHWM 1,99 GB) |

Leitura: enquanto o build é o pico do processo, o custo é **~24–25 B/entrada**;
quando o pico vem de um build anterior e só a diferença é medida, aparecem 34–42 B.
O teto observado é **~35 B/entrada emitida** (triplets 16 B + allocators ~8–25 B +
saída 8 B). É contra esse número que se avalia a Fase 1 (fragmento-CSR).

Pico de processo (`VmHWM`) por família:

| família | w1 | w2 | w3 | aloi LHRR |
|---|---|---|---|---|
| `mult` @1 CPU | 221 MB | — | 1.509 MB | — |
| `mult` @12 threads | 233 MB | — | 1.557 MB | — |
| `phases` @12 threads | 251 MB | 251 MB | 1.707 MB | — |
| `pipe` @12 threads | 511 MB | — | 3.166 MB | — |
| `real` (LHRR, k=10, L=400, T=1) | — | — | — | **4,34–4,40 GB** |

O caso real domina tudo: com uma matriz de vizinhos de 160 mil entradas
(2·N·L triplets na matriz de posição) o pico é **4,4 GB** contra 5,5 GB
disponíveis. Esse é o alvo concreto de memória da Fase 1 — não os 25–35 B
sintéticos, que têm colunas uniformes e não agrupadas por kNN.

## A4 — Harness agregado: coluna de pico de RSS real

**Diagnóstico** (o que estava errado): `execution.py` montava
`max_rss_mb = max(profiler.metrics.rss_samples)` — amostras de RSS **instantâneo do
filho direto**, que é o wrapper (`time`/`perf`/`taskset`), não o motor; e o
`rusage` só alimentava `user/sys/faults`. O agregado (`run_experiments.py`:525,
`mean_max_rss_mb`) e `result_exporter.py`:123 consumiam essa coluna. Pior: **nesta
WSL o `psutil` não está instalado e `/usr/bin/time` não existe**, então as amostras
vinham vazias (=0 MB) e o parser de `%M` nunca alimenta `max_resident_set_kb`.

**Implementação** (sem psutil, sem tocar no fluxo de execução):
- `_read_vmhwm_kb(pid)`: lê `VmHWM` de `/proc/<pid>/status` (contador monotônico).
- `_tree_peak_vmhwm_mb(sid)` + `_sample_peak_loop`: como cada filho é lançado com
  `preexec_fn=os.setsid`, **toda a árvore compartilha o session id**; o amostrador
  varre `/proc`, acha a sessão e guarda o maior `VmHWM` — mede o motor, não o
  wrapper. Thread própria, sempre iniciada (20 ms), parada no `finally`.
- `DetailedMetrics.peak_vmhwm_mb`/`vmhwm_pid`; `RunResult.peak_rss_mb` (por run) e
  `peak_ru_maxrss_mb` (`RUSAGE_CHILDREN.ru_maxrss`, teto monotônico — documentado
  como teto, não como valor por run); colunas novas `mean_peak_rss_mb`,
  `std_peak_rss_mb`, `mean_peak_ru_maxrss_mb`, `std_peak_ru_maxrss_mb` no CSV
  agregado, com as antigas preservadas.

**Validação** (`a4_check.txt`; filho que aloca ~300 MB, comparado ao `VmHWM` que o
próprio filho imprime):

| invocação | `max_rss_mb` (legado) | **`peak_rss_mb` (novo)** | `VmHWM` do filho |
|---|---|---|---|
| `sh -c "python3 child"` | 0,0 | **308,5** | 308,4 |
| `taskset -c 0 python3 child` | 0,0 | **308,6** | 308,6 |
| `python3 child` | 0,0 | **308,5** | 308,5 |

O caso com wrapper (que é o do harness) funciona porque a busca é por sessão, não
pelo filho direto. **Limitações declaradas**: `VmHWM` é por processo (com dois
filhos pesados simultâneos vale o maior); exatidão de um período de amostragem
(20 ms); `mean_max_rss_mb` continua sendo emitida — quem reprocessar CSVs antigos
deve migrar para `mean_peak_rss_mb`.

## A5 — Dados reais (ALOI, pipeline LHRR)

`Datasets/aloi/rkLists.txt` → modo `extract` (u32,
`/tmp/udlf_rk_aloi_N72000_L400.bin`, 115,2 MB) → modo `real` com
`72000 400 10 400 lhrr` (N=72.000, L=400, k=10, limit=400, T=1).

| CPUs | threads | total (ms) | VmHWM |
|---|---|---|---|
| 1 | 1 | 11.665,4 | 4,40 GB |
| 2 | 2 | 6.964,4 | 4,35 GB |
| 4 | 4 | 4.379,7 | 4,34 GB |
| 6 | 6 | 3.722,8 | 4,35 GB |
| 12 | 12 | **3.577,1** | 4,40 GB |

Speedup 1→12 threads: **3,26×** (contra 5,08× do sintético w1 — coerente com um
pipeline em que o build serial (B0/B2/B4) e o pico de memória pesam mais).

Ficha de parâmetros (`CTX`): a matriz medida é a **simétrica** do `mainMatrix`
(`rows=144.000 = 2N`), `bound_mean=846,8`, `bound_max=4.062`, `cols=15.000`,
**razão `B.cols/max_bound = 17,7`** → branch **densa**, com bound máximo 4,8× a
média; `emitted=15.972.055` numa iteração. Sintéticos para o mesmo quadro:
w1 `bound 400/400`, razão **125**, densa, 5,0M emitidos; w2 `bound 400/400`,
razão **2.500**, **hash**; w3 `bound 62.500` (100k linhas, limit 1000), razão
**1,6**, densa, 40M emitidos.

## A6 — Testes de caracterização (antes de mexer no build)

`Source/tests/test_csr_ops.cpp` (commit `9dff5cb`), **72 checks, 0 falhas**, cobrindo
o comportamento observável de `transpose`, `merge`, `combine`, `build_from_partial_results`
e do acumulador: ordenação por coluna, linhas crescentes por thread, `is_set`
(1,0 sem valor), **duplicatas** (soma/última-escrita conforme a operação), linhas
vazias, `limit` maior que o tocado, interseção de zeros, e a composição
`transpose∘transpose = identidade`. Determinismo: `md5` do dump
`b04547bd43c462daa82838f23a2d4b4a` **idêntico em 1/2/4/8/12 threads** (o cabeçalho
imprime o nº de threads e é removido antes do md5). Suíte antiga segue com
`112fb48749110da3c1cf31a5612fbe91` (1/2/4/8 threads), o mesmo md5 do baseline da
Fase 0 — ou seja, **sem regressão**.

Estágios adicionados ao `Source/tests/run_all.sh`: suíte de caracterização + o
determinismo por nº de threads, além do que já existia. Suíte completa: `rc=0`.

Nota de flags: o kernel é construído com `-ffast-math -march=native -mavx2 -mfma`
(`Source/Makefile`), enquanto os testes têm linha de compilação própria sem essas
flags. Os testes de caracterização comparam *ordem e presença*, não igualdade
bit-exata de somas reordenadas — mas qualquer teste futuro de valor numérico deve
usar tolerância ou as mesmas flags.

## O que isso muda para as Fases 1–3 (sem pré-julgar)

1. **Fase 1 (fragmento-CSR) tem alvo de memória medido**: teto ~35 B/entrada no
   sintético e, no caso real que importa, **4,4 GB de pico** em LHRR k=10/L=400/T=1,
   com o build da matriz de posição custando **1,99 GB** (57,6M triplets). O ganho
   deve ser medido com o A4 novo (agora funciona) ou com o `ΔHWM` por build.
2. **Fase 3 (`nth_element` sobre pares contíguos)**: segue justificada como antes
   (F1+F2 ≈ 95% do MULT em w3 na Fase 0), mas os números da Fase 0 em w3 estavam
   com o bind ativo; o re-profile da Fase 3 deve usar `EA_BIND=0`.
3. **Fase 4 (recalibração do `ADAPTIVE_RATIO`)**: os sintéticos cobrem razões
   1,6 / 125 / 2.500, mas o caso real cai em **17,7 com bound máximo 4,8× a média** —
   nenhum sintético cobre esse ponto, o que reforça a recalibração com matrizes
   kNN reais (agrupadas) e não com colunas uniformes.
4. **Fase 2 (transposição paralela)**: continua fora de escopo, mas note que o
   `TRANSPOSE` da Fase 0 era instrumentado no driver, e no pipeline LHRR real ele
   aparece dentro de `BUILD_CALL`/`TRANSPOSE` do ledger — o número está disponível
   para quem retomar.

## Replay (WSL, do root do repo)

```bash
python3 Source/bench/fix_crlf.py Source/bench/run_etapa_a.sh   # -> /tmp (CRLF quebra bash)
bash /tmp/run_etapa_a.sh /tmp/ea build curve_fast sib curve_w3a curve_w3b
bash /tmp/run_etapa_a.sh /tmp/ea phases_fast phases_w3a phases_w3b
bash /tmp/run_etapa_a.sh /tmp/ea pipe_w1 pipe_w3 mem
bash /tmp/run_etapa_a.sh /tmp/ea real          # precisa do .bin do modo extract
python3 Source/bench/aggregate_etapa_a.py /tmp/ea/raw > agregado.txt
# knob de binding: EA_BIND=1 reproduz a campanha v1 (com OMP_PROC_BIND/PLACES)
# reproduções pontuais: Source/bench/_bindcheck.sh, _noisecheck.sh, _decisive.sh,
#                       _replicate.sh, _maskprobe.sh, _probe_bind.sh, _a4_check.py
```

## Caveats

- WSL2: sem `perf` de hardware e sem `/usr/bin/time`; ruído ±5% (os pontos de
  1 CPU da v2 saíram 4–9% acima do re-check ocioso).
- `max_concurrency`/`affinity_omp` são lidos **depois** de uma região paralela
  (por isso os campos `affinity_at_entry` e `req_mask` existem); qualquer medição
  futura deve conferir esses campos antes de aceitar um ponto.
- Os sintéticos usam colunas uniformes; o real usa grafo kNN (agrupado). Ambos
  estão no mesmo agregado e não devem ser misturados (a coluna `family` separa).
- `Datasets/` não entra no git (`.gitignore`); o `.bin` extraído vive só em `/tmp`.
