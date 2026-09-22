# Etapa B / item 1 — A5-bis: **a Fase 1 medida no LHRR real (ALOI)**

Validação da Fase 1 no caso que domina o pipeline real, com o mesmo harness e o mesmo
protocolo da Etapa A (masks `0/0-1/0-3/0-5/all`, `taskset`, sem bind, 5 reps,
mediana dos reps 1..n com rep 0 descartado).

- Config: **LHRR real, ALOI**, `N=72000, L=400, k=10, limit=400, T=1`, `.bin` u32
  extraído do `Datasets/aloi/rkLists.txt` (`max_id=71999 < N`, 115,2 MB).
- Kernel: `4e57526` (estado final = Fase 1). Referência: capturas `real_lhrr_aloi_*`
  da Etapa A (`Data/phase_profile/etapa_a/raw_v2/`, kernel pré-Fase 1).
- Artefatos: `raw/` (5 capturas de tempo), `raw_mem/` (5 capturas com `PHASE_MEM=1` +
  o `extract`), `aggregate.txt` (`Source/bench/a5bis_agg.py`), `mem_table.txt`
  (`Source/bench/a5bis_mem.py`).

## Resultado principal

| T | ref (pré-Fase 1) | bis (Fase 1) | speedup | ref VmHWM | bis VmHWM | Δ |
|---|---|---|---|---|---|---|
| 1 | 11 665,4 ms | 10 944,3 | 1,07× | 4 299 MB | 3 857 | **−10,3%** |
| 2 | 6 964,4 | 6 085,1 | 1,14× | 4 246 | 3 847 | −9,4% |
| 4 | 4 379,7 | 3 743,1 | 1,17× | 4 242 | 3 880 | −8,5% |
| 6 | 3 722,8 | 3 342,3 | 1,11× | 4 250 | 3 879 | −8,7% |
| 12 | 3 577,1 | 3 154,6 | 1,13× | 4 299 | 3 904 | −9,2% |

`spread%` intra-captura: 4,8-8,7% no ref, 10-32% no bis em 1 thread (sessões diferentes,
~13 h de intervalo) — ver "Ruído" abaixo.

## Onde o ganho apareceu (ledger, mediana dos reps, ms)

| etapa | ref@1t | bis@1t | ref@12t | bis@12t | ganho@12t | % do bis@12t |
|---|---|---|---|---|---|---|
| **`position_build`** | 7 105 | **8 324** | 2 390 | **2 605** | **0,92×** | **82,6%** |
| `update_ranked_lists_pos` | 1 339 | 1 375 | 146,7 | 146,6 | 1,00× | 4,6% |
| `H_build` | 579 | 629 | 114,0 | 162,7 | 0,70× | 5,2% |
| `H_transpose` | 8,3 | 14,5 | 9,0 | 11,9 | — | 0,4% |
| `mult_S` | 743,1 | 276,8 | 250,2 | 69,9 | **3,6×** | 2,2% |
| `mult_Srev` | 1 045,8 | 324,1 | 355,4 | 80,4 | **4,4×** | 2,5% |
| `combine_S_total` | 363,4 | 194,6 | 112,5 | 27,0 | **4,2×** | 0,9% |
| `combine_main` | 381,0 | 86,0 | 112,9 | 17,1 | **6,6×** | 0,5% |
| `update_ranked_lists` | 149,7 | 38,6 | 17,3 | 7,2 | 2,4× | 0,2% |

Soma @12t: ref 3 539,6 → bis 3 164,9 (−375 ms, −10,6%), coerente com o wall.

## A descoberta: o gargalo dominante é o **caso de exceção da Fase 1**

`position_build` **não melhorou** (2 605 vs 2 390 ms @12t; 8 324 vs 7 105 @1t) porque
ele **não passa pelo fragment-CSR**: emite `t2->row = neighbor` (triplet simétrico) sob
`#pragma omp for schedule(dynamic)`, então **as linhas caem fora do range da thread** e
o build usa `build_from_partial_results` — o caminho global (`temp_triplets_` + sort
global + histograma). É literalmente o caso que o driver documenta como "the Phase-1
exception case" (`bench_phase.cpp:358-360`) e o que `LHRRSparse.hpp:73-81` mantém.

A prova está no **ledger de memória** (`raw_mem/`, `MEMBUILD`, `clear_refs=5` antes de
cada build, mediana de 3 reps):

| n | build | emitted | tmp_bytes | out_bytes | ΔHWM (MB) | **B/entrada** | caminho |
|---|---|---|---|---|---|---|---|
| 1 | position (N×N simétrica) | 57 600 000 | **921,6 MB** | 321,7 MB | **1 938,8** | **35,3** | **GLOBAL** |
| 2 | H | 7 272 000 | **116,4 MB** | 15,9 MB | **232,0** | **33,5** | **GLOBAL** |
| 3 | mult_S | 7 286 865 | 0 | 58,3 MB | 55,5 | **8,0** | frag-CSR |
| 4 | mult_Srev | 8 685 190 | 0 | 69,5 MB | 66,2 | **8,0** | frag-CSR |
| 5 | combine_S_total | 1 448 162 | 0 | 11,6 MB | 11,0 | **8,0** | frag-CSR |
| 6 | combine_main | 1 356 712 | 0 | 10,9 MB | 10,5 | **8,1** | frag-CSR |

Os `tmp_bytes` reproduzem exatamente o número citado no README da Etapa A para este
caso (35,3 B/entrada; ΔHWM 1,99 GB), o que confirma que o caminho é o mesmo de antes.
**Caminho global: 33-35 B/entrada emitida. Fragment-CSR: 8,0 B/entrada** — 4,3× menos,
mais os 921,6 MB de buffer temporário da matriz de posição, que só existem ali.

Consequência: depois da Fase 1 o gargalo ficou **mais** concentrado — `position_build`
saiu de 66,8% para **82,6%** do pipeline @12t (76,1% @1t) — e os builds que a Fase 1
toca (mults/combines) somam hoje só **6,1%** do pipeline @12t.

## Ruído (limite de resolução desta campanha)

- **@1t o ganho não é demonstrável**: as reps do bis vão de 10,6 a 14,0 s (32% de
  spread) contra 11,6-12,0 s do ref (4,8%). O 1,07× está dentro do ruído — o que vale
  afirmar é o efeito por etapa e a memória.
- **@12t o ganho é sustentado**: ref 3 486-3 663 ms (4,8%), bis 2 947-3 246 (10%); o
  ledger fecha −375 ms de soma de etapas, com cada mult/combine 3,6-6,6× mais rápido —
  muito acima do spread.
- `position_build`, `H_build` e `update_ranked_lists_pos` **não mudaram de código** e as
  diferenças observadas (+9%/+43%/+17%) ficam dentro da variabilidade rep-a-rep do
  próprio estágio (só no bis@1t o `B2_SORT` varia de 6,4 a 9,0 s). Não atribuir à
  Fase 1. Corolário: comparações futuras desse estágio precisam de mais reps, ou de
  CPU-time (`user_ms`) em vez de wall.

## O que isso define como próximo alvo (com números)

Se o `position_build` passar para o fragment-CSR:

- **memória**: `tmp_bytes` (921,6 MB) desaparece e o ΔHWM do build cai de 1 938,8 MB
  para ~1,0-1,2 GB — a arena de 922,7 MB **continua** (a emissão ainda materializa
  57,6M triplets) e a saída é 321,7 MB. Pico do processo: ~3,86 → **~2,9-3,2 GB**.
- **tempo @12t**: o wall do `position_build` (2 605 ms) está ~95% dentro de
  `BUILD_CALL`, dominado por `B2_SORT` (~1 700-1 800) + `B0_TEMP_RESIZE` (~420) +
  `B4` (~218) + cópia/fill. No fragment-CSR a mesma matriz custa histograma + prefixo +
  fill (no w3 sintético, 20M emitidos em 1 thread ⇒ `BUILD_CALL` 228 ms); extrapolando
  para 57,6M e 12 threads, **ordem de 150-300 ms**. O pipeline @12t cairia de ~3 155 ms
  para **~1,0-1,2 s** (2,6-3,1×) e o gargalo migraria para
  `update_ranked_lists_pos` / `H_build` / `mult_Srev`.
- **Como fazer, sem violar o invariante** (o emissor precisa emitir linhas que a thread
  possui): (a) particionar por linha de destino e rotear o triplet para o allocator do
  dono (hoje o allocator é por thread, então exige troca de protocolo); (b) construir
  `P_fwd` com emissão row-local (fragment path) e obter a parte simétrica por
  `merge(P_fwd, transpose(P_fwd))` — as duas operações já estão no caminho rápido
  medido (o `H_transpose` real custa 9-15 ms; para a matriz de posição, ~2× maior, o
  custo do transpose precisa ser medido antes); (c) paralelizar o próprio
  `build_from_partial_results` (bucketing/radix paralelo em vez do `std::sort` global),
  o que resolve todos os casos de exceção de uma vez. **(b) é a de menor resistência**:
  reaproveita dois caminhos já medidos; **(c) é a de maior alcance**.

## Caveats

- **Não existe decomposição de memória pré-Fase 1 para o `real`**: as capturas
  commitadas da Etapa A (`raw/` e `raw_v2/`) não têm `MEMBUILD` (só os sintéticos w1/w3
  têm). A tabela por build acima é **bis-only**; a comparação de pico de processo
  (VmHWM) bis-vs-ref está na primeira tabela.
- `/tmp` no WSL é **tmpfs**, limpo a cada restart: o `.bin` extraído (115,2 MB, 84-87 s
  de extração) foi perdido no meio desta campanha e teve de ser re-extraído. O fluxo
  agora cacheia em `/home/andre/udlf_cache/` (ext4, persistente) e só re-extrai em
  cache miss.
- `PHASE_MEM=1` reseta o VmHWM (`clear_refs=5`) antes de cada build, então o `DONE` das
  capturas `_mem` é "pico desde o último reset" (1 348-1 453 MB) e **não** o pico do
  processo. Use as capturas de tempo (sem `PHASE_MEM`) para pico real.
- `EA_MEM`/`EA_BIN` foram adicionados ao harness com default inerte
  (`EA_MEM=0`, `EA_BIN=/tmp/udlf_rk_aloi_N72000_L400.bin`): as capturas da Etapa A
  continuam reproduzíveis com o mesmo script.

## Reprodução

```bash
# 1. harness (default EA_MEM=0 mantém as capturas da Etapa A reproduzíveis)
python3 Source/bench/fix_crlf.py Source/bench/run_etapa_a.sh
# 2. extração (cache persistente -- /tmp é tmpfs)
/tmp/bench_a_s1 extract Datasets/aloi/rkLists.txt 72000 400 \
    /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
# 3. tempo + ledger (5 reps, taskset, sem bind)
bash /tmp/run_etapa_a.sh /tmp/ea_bis build real
# 4. memória (PHASE_MEM=1, 3 reps, tags *_mem)
EA_MEM=1 EA_BIN=/home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin \
    bash /tmp/run_etapa_a.sh /tmp/ea_bis_mem real
# 5. tabelas
python3 Source/bench/a5bis_agg.py Data/phase_profile/etapa_b/a5bis/raw \
        Data/phase_profile/etapa_a/raw_v2
python3 Source/bench/a5bis_mem.py Data/phase_profile/etapa_b/a5bis/raw_mem
```

