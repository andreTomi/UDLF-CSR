# Etapa B / item de corretude — **o branch Dense emitia colunas fora de ordem**

Status: **bug confirmado, corrigido e validado in-run**. Descoberto pela revisão do
`multiply_prune_row` (não pelos testes), reproduzido por teste, corrigido
incondicionalmente, e **provado ter corrompido o resultado real da A5-bis**.

## O bug (código, antes da correção)

`DenseRowAccumulator::accumulate` faz `active_.push_back(col)` no **primeiro toque** →
`used()` está em ordem de primeiro toque. `col_of(slot) == slot` significa apenas que o
slot **é** a coluna — não que a lista esteja crescente. O `multiply_prune_row` da Fase 1
ordenava o prefixo emitido **só** para o acumulador Hash:

```cpp
if constexpr (!std::is_same_v<Acc, DenseRowAccumulator>) {
    std::sort(used.begin(), used.begin() + emit_count, by col_of);
}
```

Para uma linha de A com dois `k` cujas linhas de B se sobrepõem fora de ordem
(`k=0 → cols {1,3,5}`, `k=1 → cols {0,2,3}`), o `active_` final é `[1,3,5,0,2]` — e o
`nth_element` (quando há poda) embaralha ainda mais. O `build_from_ordered_fragments`
exige apenas **row-monotonicidade** (cursor linear por thread), então `row_ptr_` fica
correto e `col_idx_` sai desordenado. `get()` é `lower_bound` e `merge()` é
two-pointer → **corrupção silenciosa de valores**. Pré-Fase 1 isso era seguro: o
builder global ordenava por (row,col) antes de construir.

## 1. Teste (C18 em `test_csr_ops.cpp`) — falhou antes, passa depois

Caso: 1 linha de A com 2 k's; B com colunas sobrepostas fora de ordem; branch Densa
forçada (`B.cols 6 <= 200*max_bound 6`); checagem por `csr_well_formed` (ordem crua de
`col_idx_` via `for_each_in_row`).

| caso | emitido pré-fix | pós-fix | pré-fix |
|---|---|---|---|
| não podado (`limit=10`, touched=5) | `1 3 5 0 2` | `0 1 2 3 5` | **FAIL** |
| podado (`limit=3`, touched=5) | `2 3 1` | `1 2 3` | **FAIL** |
| podado grande (25 tocados, limit=20) | — | — | **FAIL** |

`prefix_fail.txt`: **77 checks, 3 failures** · `postfix_pass.txt`: **77 checks, 0
failures**. (O md5 do `test_csr_ops` muda por design — o caso novo imprime linhas.)

## 2. Correção

- `multiply_prune_row`: os **dois** acumuladores ordenam o prefixo emitido. Dense:
  `std::sort(used.begin(), used.begin()+emit_count)` — inteiro puro, sem comparador
  indireto (`col_of(slot) == slot`). Hash: comparador por `col_of` como antes.
- **Asserção padrão** (`#ifndef NDEBUG`) em dois pontos: no `multiply_prune_row`
  (prefixo emitido ascendente) e no **fill do `build_from_ordered_fragments`** (linhas
  não-decrescentes + colunas estritamente crescentes — o ponto por onde passam todos
  os produtores do caminho de fragmentos). Builds de debug e os testes (que não usam
  `-DNDEBUG`) quebram se o invariante for reintroduzido; as capturas cronometradas
  usam `-DNDEBUG` (harness atualizado) para não pagar o custo.
- `merge`/`combine`: `t->is_set = false` em todas as 7 emissões (o campo carregava
  lixo da arena; hoje inócuo porque o builder de fragmentos o ignora, mas é uma
  bomba-relógio para qualquer consumidor do builder global).

## 3. Prova de corrupção na ALOI real (validação in-run, `PHASE_VALIDATE=1`)

Novo knob no driver (`VALIDATE,<matriz>,ok|FAIL,<motivo>`), rodado no pipeline LHRR
real (12 CPUs, 1 rep):

| matriz | pré-fix | pós-fix |
|---|---|---|
| S | **FAIL** — columns not strictly increasing | ok |
| Srev | **FAIL** | ok |
| Stot | **FAIL** | ok |
| result | **FAIL** | ok |
| **REP** | nnz **1 356 712**, checksum **47 307 288 119.15** | nnz **5 248 535**, checksum **119 396 289 001.12** |

O pós-fix reproduz **exatamente** o resultado do kernel pré-Fase 1 (correto). Ou seja:
**a A5-bis mediu um pipeline corrompido** — o `combine` perdeu ~3,9M células (nnz 1,36M
vs 5,25M) porque o `lower_bound` falhava em linhas desordenadas, e o `result` final
herdou as perdas. A tabela da A5-bis está **superada** (banner em `../a5bis/README.md`);
as capturas corretas são `../a5bis/raw_fix/`.

## 4. A5-bis corrigida — a conclusão sobre a Fase 1 mudou

Com o kernel correto (`../a5bis/aggregate.txt`, bloco raw_fix vs Etapa A):

| T | pré-Fase 1 | Fase 1 corrigida | razão |
|---|---|---|---|
| 1 | 11 665,4 ms | 10 955,6 | 1,06× |
| 2 | 6 964,4 | 6 527,0 | 1,07× |
| 4 | 4 379,7 | 4 268,3 | 1,03× |
| 6 | 3 722,8 | 3 859,1 | **0,96×** |
| 12 | 3 577,1 | 4 226,8 | **0,85×** |

**No pipeline real, a Fase 1 é paridade @1-4t e regressão @6-12t.** O "ganho de
1,07-1,17×" da A5-bis era artefato da corrupção (o `combine` fazia menos trabalho).
O custo da correção aparece no F2: @1t 98,6 → 482,4 ms (o sort intralinha de até
`limit` inteiros por linha) e o VmHWM sobe para ~4,0 GB (o resultado correto tem
5,25M células).

Comparação que **sobrevive** ao ruído entre sessões (intra-sessão, mesmo tamanho
emitido, 7,3M): `mult_S` no caminho de fragmentos 105,1 ms vs `H_build` no caminho
global 218,2 ms → **o builder de fragmentos é ~2,1× mais rápido por entrada**
(consistente na sessão pré-fix: 69,9 vs 162,7 → 2,3×).

## 5. Custo da correção (sintéticos, 1 core, 5 reps, mediana dos reps 1..n)

| workload | F2 pré | F2 pós | total pré | total pós |
|---|---|---|---|---|
| w1 (Dense, limit 50) | 177,1 ms | 241,7 ms (**+36%**) | 395,8 | 484,7 (+22%) |
| w3 (Dense, limit 1000) | 8 309 ms | 8 598 ms (**+3,5%**) | 19 109 | 18 705 (−2%, ruído) |

(`cost_pre_*`/`cost_post_*` nesta pasta; w1 tem spread de 15-33% entre rodadas, então
o +22% de wall é parcialmente ruído — o sinal limpo é o **+36% de F2**.)

## 6. Item 4 — matriz de bind (`{bind, sem bind} × {base, HEAD}`, 12 CPUs, 3 reps)

| kernel | bind | TBB max | real (ms) | `position_build` | w1 (ms) |
|---|---|---|---|---|---|
| base (86898d5) | 0 | 12 | 5 259,4 | 3 924,7 | 195,8 |
| base | **1** | **2** | **8 606,0 (+64%)** | 6 137,7 | 212,9 |
| HEAD | 0 | 12 | 4 195,7 | 3 456,2 | 85,9 |
| HEAD | **1** | **2** | **7 245,4 (+73%)** | 5 435,1 | 114,0 |

`TSET` auto-documenta (`max_concurrency=2` com bind). O efeito é **muito maior no
pipeline real** que no w1 sintético (+64-73% vs +9-33%) porque o sort global TBB da
matriz de posição (57,6M triplets) domina. A coluna GraphBLAS fica fora: a stack
(GraphBLAS/MKL/jemalloc) não está instalada nesta WSL e a comparação final deve rodar
no ambiente original da tese.

## 7. Caveat de estabilidade de medição (importante para a tese)

O **mesmo** kernel/config (`base`, sem bind, real@12t) mediu **5 259 ms** nesta sessão
contra **3 577 ms** na sessão da Etapa A (+47%); o `position_build` (código idêntico)
variou 2 605 → 3 232 entre as sessões da Etapa B (+24%). Consequências: (a) diferenças
end-to-end < 25% **não** são resolíveis entre sessões; (b) a leitura "Fase 1 = paridade
@1-4t, regressão @6-12t" precisa de um A/B na mesma sessão (o único par same-session
disponível é o da matriz de bind, sem-bind: HEAD 4 195,7 vs base 5 259,4 → 1,25× @12t,
mas confundido pela ordem de execução/warm-up); (c) os deltas por etapa grandes são
robustos. Qualquer número da tese deve vir de A/B same-session ou de muitas sessões.

## 8. VEREDITO FINAL — A/B same-session intercalado (`s1_ab/`)

O A/B que o item (b) exigia foi executado conforme o desenho acordado: 6 blocos
intercalados × 2 reps (**base → head → head → base → base → head**), mesma sessão,
mesma janela de ~3 min, `taskset -c 0-11`, kernels compilados dos commits `86898d5`
(base) e `4e57526` (head, com o fix de ordenação) — **ambos produzem o resultado
correto** (nnz 5 248 535 / checksum 119 396 289 001.122040, idêntico entre si e ao
pré-Fase 1). Canário de carga fixa antes/depois; probe de resfriamento (180 s ocioso).

| bloco | kernel | ms (mediana das 2 reps) |
|---|---|---|
| 1 | base | 5 026,1 |
| 2 | head | 3 671,5 |
| 3 | head | 3 669,8 |
| 4 | base | 4 247,5 |
| 5 | base | 4 194,3 |
| 6 | head | 3 487,6 |

- **Global**: base 4 247,5 vs head 3 669,8 → **head/base = 1,16×**
- **Pareado por posição** (cada head vs a média dos blocos base adjacentes):
  **1,263 / 1,264 / 1,203 → mediana 1,26×**
- **Canário** (w1, carga fixa, mesmo binário, antes/depois): 78,7 → 85,0 ms
  (**+8,0%** de deriva na sessão — muito abaixo do efeito medido)
- **Probe de throttle**: cold-base após 180 s ocioso = 4 392,7 ms (**+3,4%** vs warm)
  → **sem assinatura de throttling acumulado**; o padrão dos blocos base
  (5 026 → 4 247 → 4 194) é **warm-up** (melhora com o tempo), não degradação.

**Conclusão**: no pipeline LHRR real (ALOI, L=400, T=1), **a Fase 1 corrigida é
~1,2-1,3× mais rápida que o kernel pré-Fase 1 @12t**. A "regressão @6-12t" (0,85×)
da seção 4 era **arte-fato de comparação entre sessões** — exatamente o que o caveat
da seção 7 alertava, e que eu, incorretamente, apresentei como conclusão. Retração
registrada. A leitura correta e final:

- Fase 1 corrigida: **mais rápida no real @12t (1,2-1,3×), com pico de memória
  semelhante (~4,0 GB vs 4,3 GB)**;
- o gargalo dominante continua sendo o `position_build` no caminho global (Builder
  v2 / rota (c)), agora com o custo do builder global **e** o do fragmento medidos
  na mesma sessão (2,1× por entrada a favor do fragmento).

Agora sim os números de tempo do pipeline real estão aptos a embasar a decisão
(b) vs (c) — embora a decisão permaneça motivada pela arquitetura (um contrato de
emissão único), não pelo tempo.


## Arquivos

`prefix_fail.txt`/`postfix_pass.txt` (C18) · `val_pre_aloi.txt`/`val_post_aloi.txt`
(VALIDATE in-run) · `cost_pre_*`/`cost_post_*` (custo do sort) · `bind_matrix.txt` +
`bind_raw/` (item 4) · `postfix_a5bis_t12.txt` (nnz/checksum corretos). Agregador:
`Source/bench/bind_agg.py`; validação no driver: `PHASE_VALIDATE=1`.

