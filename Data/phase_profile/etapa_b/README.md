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
| md5 do resultado idêntico ao pré-mudança em 1/2/4/8/12 threads | ✅ `f1`, `f3b` e `fin`: `b04547bd43c462daa82838f23a2d4b4a` / `112fb48749110da3c1cf31a5612fbe91` |
| Testes passando | ✅ 72 checks/0 falhas · ALL TESTS PASSED (1..12 threads) |
| ΔHWM e tempo de build (CPU, 1 core) antes/depois | `BUILD_CALL` 153,6 → **12,5** ms (w1) · 145,9 → **13,0** (w2) · 2384,1 → **227,7** (w3) = **11-15×**; HWM 207,6 → **143,5** MB (w1) · 190,1 → **126,2** (w2) · 654,8 → **408,9** (w3) = **−25%..−38%** |
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
