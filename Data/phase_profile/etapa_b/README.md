# Etapa B — resultado

Escopo executado: **Fase 1** (fragment-CSR por thread) e **Fase 3** (`nth_element` sobre
pares contíguos), em commits separados, cada uma com reprofiling no mesmo driver.
Fases 2, 4 e 5 ficaram fora de escopo por decisão (ver README da Etapa A).

| Fase | Status | Evidência principal |
|---|---|---|
| **1** — fragment-CSR por thread | **mantida** | build 11-15× mais rápido, HWM −25..−38%, wall −11..−28%; md5 e suites idênticos |
| **3** — `nth_element` sobre pares contíguos | **rejeitada** | F2_PRUNE **+48%** (w1), +8..16% (w2), +8..12% (w3), **+15,6%** (w4) — pior na fase que existia para melhorar |
| **3b** — materialização condicional | **rejeitada** | empata no único caso em que difere de `f1` (w2) e é **+15,6%** em F2 no w4; em w1/w3 é o mesmo caminho de código |

Detalhe da investigação, método, controle de ruído e tabelas completas:
`fase3/README.md`. Brutos: `ab_raw/` (19 arquivos) e `ab_raw/summary.txt`.
Fase 1: `fase1/README.md`.

## Bug de ordenação do Dense (corrigido — lê-se primeiro)

A Fase 1 ordenava o prefixo emitido só no acumulador Hash, com o argumento de que
`col_of(slot) == slot` implica ordem crescente no Dense. **Falso**: `active_` está em
ordem de primeiro toque, e linhas de A com ≥2 `k` emitiam colunas desordenadas — o que
corrompia silenciosamente `combine` (`get`/`lower_bound`) e `merge` (two-pointer), e
**tornou inválida a A5-bis** (o pipeline real perdeu ~3,9M células). Correção +
asserções `#ifndef NDEBUG` + teste C18 + validação in-run (`PHASE_VALIDATE=1`) em
`6d1c86e`; análise completa, prova de corrupção, custo da correção e matriz de bind em
`ordem/README.md`. A seção A5-bis abaixo está **superada**. **Veredito de tempo
(same-session, `ordem/README.md` §8)**: com o A/B intercalado ABBA + canário, a Fase 1
corrigida é **1,16× (global) / 1,26× (pareado) mais rápida** que o kernel pré-Fase 1
no real @12t — a "regressão @6-12t" anterior era artefato de comparação entre sessões
(retratada). As rotas (b)/(c) para o `position_build` seguem bloqueadas para decisão,
agora com o embasamento de tempo same-session disponível.

## A5-bis — a Fase 1 medida no LHRR real (ALOI)

> **SUPERADO**: as capturas `a5bis/raw/` foram geradas pelo kernel pré-correção de
> ordenação e **produziam um resultado corrompido** (nnz 1 356 712 vs 5 248 535
> corretos; checksum 47,3e9 vs 119,4e9) — o `combine` perdeu células porque o branch
> Dense emitia colunas desordenadas (bug e correção em `ordem/README.md`). Os números
> abaixo são de uma computação que perdeu ~3,9M células e **não** devem ser citados.
> Versão corrigida: `a5bis/raw_fix/` + `ordem/README.md` — conclusão revisada:
> **paridade @1-4t e regressão @6-12t** vs o kernel pré-Fase 1 (não o ganho de
> 1,07-1,17× listado aqui).

Mesma campanha da Etapa A (`real`, ALOI `N=72000, L=400, k=10, limit=400, T=1`,
`taskset`, sem bind, 5 reps, mediana dos reps 1..n), agora com o kernel da Fase 1.
Resultado (`a5bis/README.md`, `a5bis/aggregate.txt`, `a5bis/mem_table.txt`):

- **wall**: 1,07× @1t · 1,14× @2t · **1,17× @4t** · 1,11× @6t · 1,13× @12t;
- **pico de memória (VmHWM)**: 4,30 GB → **3,86-3,90 GB** (−8,5%..−10,3%);
- **etapas**: `mult_S` 3,6× · `mult_Srev` 4,4× · `combine_S_total` 4,2× ·
  `combine_main` 6,6× (todas @12t), coerentes com a Fase 1 (fragment-CSR);
- **o gargalo dominante não melhorou porque não passa pela Fase 1**:
  `position_build` emite `t2->row = neighbor` sob `schedule(dynamic)` ⇒ linhas fora do
  range da thread ⇒ `build_from_partial_results` (caminho global). É o caso de exceção
  documentado em `LHRRSparse.hpp:73-81` e em `bench_phase.cpp:358-360`. Prova pelo
  ledger de memória: esse build (e o `H_build`) mostra `tmp_bytes = 921,6 MB`
  (buffer global) e **35,3 B/entrada emitida**, contra **8,0 B/entrada** nos builds que
  usam fragment-CSR (`tmp_bytes = 0`). Depois da Fase 1 o `position_build` passou a ser
  **82,6%** do pipeline @12t.
- **próximo alvo, com números**: mover esse build para o fragment-CSR vale ~2,1-2,3 s
  @12t (pipeline ~3 155 → ~1,0-1,2 s) e ~0,9 GB de pico (→ ~2,9-3,2 GB). Opções (b)
  `merge(P_fwd, transpose(P_fwd))` — menor resistência — ou (c) paralelizar o próprio
  builder global — maior alcance. Detalhes em `a5bis/README.md`.


## Estado final

O kernel final é o da **Fase 1**: `Source/src/Core/hpp/SparseMatrix.hpp` restaurado de
`0fba99c` (commit `4e57526`, diff vazio contra aquele commit). Sem `pairs`, sem
materialização, `nth_element` indireto como na Fase 1. Reverificação no estado final:

- diff contra `0fba99c`: **vazio**;
- md5 das duas suítes **idêntico ao baseline** a 1/2/4/8/12 threads
  (`b04547bd…` · `112fb487…`);
- `test_csr_ops` 72 checks / 0 falhas · `test_multiply_and_prune` ALL TESTS PASSED;
- perfil `fin` (3 reps, `taskset -c 0`): w1 `373,6 ms` / F2 `168,1` · w2 `498,5 ms` /
  F2 `230,7` — dentro da banda de `f1` (w1 395,8; F2 174,8 · w2 531,2; F2 242,2), como
  esperado de código idêntico.

## Critérios de aceite da Etapa B

| Critério | Resultado |
|---|---|
| md5 do resultado idêntico ao pré-mudança em 1/2/4/8/12 threads | ✅ `f1`, `f3b` e `fin`: `b04547bd43c462daa82838f23a2d4b4a` / `112fb48749110da3c1cf31a5612fbe91` — **retratação**: o md5 hasheia lista-de-casos + pass/fail, **não** valores nem ordem (nenhuma suíte imprime matriz). Prova de valores = `same_result`/`to_map`; prova de ordem = C18 + asserções `#ifndef NDEBUG` (adicionadas em `6d1c86e`) |
| Testes passando | ✅ 72 checks/0 falhas · ALL TESTS PASSED (1..12 threads) |
| ΔHWM e tempo de build (CPU, 1 core) antes/depois | `BUILD_CALL` 153,6 → **12,5** ms (w1) · 145,9 → **13,0** (w2) · 2384,1 → **227,7** (w3) = **11-15×**; **VmHWM real** (linha `DONE`, capturas sem `PHASE_MEM`): w1 **157,7 → 118,8 MB** (−24,7%) · w2 **157,6 → 118,6** (−24,7%) · w3 **1 337,3 → 1 337,6** (inalterado — o pico de w3 são as matrizes, não o build). A coluna "HWM_MB" do resumo A/B original estava **errada** (lia `rss_final_kb`, RSS instantâneo) — corrigida em `ab_agg.py`/`summary.txt` |
| `merge`/`combine` emitem colunas ordenadas; só `multiply_and_prune` ordena por linha | ✅ `merge`/`combine` usam o mesmo builder ordenado; `multiply_and_prune` ordena ≤ `limit` entradas por linha (Dense: nenhuma ordenação, pois `col_of(slot) == slot`) |
| Sem `std::vector` + `push_back` por thread | ✅ emissão via `BlockAllocator` (arena `PaddedAllocator`); `build_from_ordered_fragments` faz histograma + prefix-sum + `resize` **exato** e escreve direto no CSR final — nenhum buffer global, nenhum pico transitório de realocação |
| API de triplets e caminho global mantidos para o `mainMatrix` simétrico do LHRR | ✅ `build_from_partial_results` preservado para o build simétrico de `LHRRSparse` (emite `row = neighbor`, fora do intervalo da thread) |

## Caveats

- **Ruído**: em w1/w2 (runs de ~0,4-0,6 s) o `spread%` entre rodadas chega a 15-37% e
  fases que nenhuma variante tocou variam ±20-30%. Só w3/w4 (18-30 s) resolvem
  diferenças de fase; w1/w2 decidem apenas por ordem de grandeza.
- **`checksum` do driver é invariante fraco** (soma `float` em `double`, 6 decimais,
  ordem intra-linha não verificada). O md5 das suítes é o critério forte.
- **HWM por fase** (`PHASE_MEM=1`) é o pico *desde o último reset* via
  `/proc/self/clear_refs`, portanto sensível ao número de reps; o pico real do processo
  é o da linha `DONE` / do harness A4.
- **Workloads sintéticos têm colunas uniformes** (não-kNN); w4 foi desenhado para o
  regime de tabela Hash que estoura a L1d, mas partição real de kNN pode alterar o
  balanço cache/gather.
