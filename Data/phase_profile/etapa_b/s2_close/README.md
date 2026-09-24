# S2 — encerramento (S2.1 → S2.4): builder único, a regressão e a correção

Consolidação do arco S2 (contrato de construção de matrizes) para servir de
referência aos métodos novos e à seção de metodologia da tese. O detalhe de
cada passo fica nos READMEs de subpasta; aqui só a decisão, o mecanismo, o
contrato final e as lições.

| passo | artefato | o que fechou |
|---|---|---|
| **S2.1** | `../s2/README.md` | Builder **v2** (único): histograma→cursores→scatter→sort→compactação; `tmp_bytes` 921,6 MB → **0**, ΔHWM do build **−77%**, `nnz` previsto == medido |
| **S2.2** | `../s22b/README.md` | A/B **runtime** `PHASE_BUILDER=legacy\|v2` no pipeline real (mesmo binário, mesmos headers): v2 **1,89–1,93×** mais rápido em T=1/2/12 |
| **S2.3** | `../s23/README.md` | A/B das variantes de ordenação por linha (idx/zip) e do preparo da entrada; viraram knob (`PHASE_ROWSORT`) |
| **S2.3 p2** | `../s23p2/` (brutos) + `../s23/README.md` | **decisão (A) EXECUTADA**: remoção dos construtores legado + fragment-CSR → builder único v2; **regressão** medida (+8,6% T=1, +1,3–3,0% T=12, **+21,9%** CPRR) |
| **S2.4** | `../s24/README.md` | Fase 0 = diagnóstico ABBA (**H1**); Fase 1 = **fast path do V4** com critério estrito → regressão **recuperada à paridade** |

## 1. A decisão (A) — o que foi decidido e o que foi executado

O objetivo declarado do S2 era um **contrato único de construção**: *um método
novo não precisa conhecer invariantes de emissão*. Existiam dois caminhos
(construtor legado + fragment-CSR por thread, com `temp_triplets_` global) e a
decisão **(A)** foi **remover os dois** e ficar só com o **builder v2**
(order-agnostic), executada no **S2.3 p2**.

- O que se ganhou, medido: `tmp_bytes` 921,6 MB → **0** (sem buffer global, sem
  TBB, **sem sensibilidade a bind** no builder); ΔHWM do build **−77%**
  (`bytes_per_triplet` 35,3 → 8,0); e, no A/B do S2.2, o v2 já era
  **1,89–1,93×** mais rápido que o legacy no pipeline real.
- O preço, medido **depois** da remoção: **regressão de tempo** de +8,6% (T=1),
  +1,3–3,0% (T=12, LHRR) e **+21,9%** (CPRR). A estimativa que justificou a
  decisão ("1–1,5%") foi produzida na revisão humana, fora do repo, e **está
  refutada** — registro canônico em `../s23/README.md`.

## 2. O mecanismo da regressão e a correção (S2.4)

**Diagnóstico (Fase 0, `bp_pre_p2` vs `bp_s23` sem fast path):** o delta
concentra-se em `BUILD_CALL` (H1). Em T=1 LHRR, `V4_ROWSORT` sozinho explica
quase todo o excesso (+230 ms CPU de +395 ms de `BUILD_CALL`). Causa: a troca
Frag→V2 roteou mult/combine/merge pelo v2, que roda **`V4_ROWSORT`
incondicionalmente sobre uma entrada que já chega ordenada** (os resultados de
mult saem ordenados desde a correção do C18; combine/merge são two-pointer).

**Correção (Fase 1):** predicado público `v2_row_is_presorted()` com critério
**estrito** `!(cols[i-1] < cols[i])` — falha em desordem **e** em duplicata
adjacente — antes dos dois modos de sort. Só pula quando a chave é única, e aí
"pular" == "ordenar" **por prova** (chave única ⇒ única ordem crescente ⇒
mesmo array). Linha com duplicata cai sempre no sort completo = comportamento
pré-Fase 1 bit-a-bit. O builder **verifica a própria entrada** — não confia em
hint de produtor.

| modo (Fase 1: f0 → f1) | ratio | ABBA | Δ`V4_ROWSORT` (CPU) | leitura |
|---|---|---|---|---|
| abcprr T=12 | 1.219 → **0.828** | −16,3 ± 11,6 ms | −77,2 | ganho relevante |
| ab1 T=1 | 1.066 → **0.960** | −366,2 ± 136,3 ms | −196,4 | ganho grande (CPU ≈ wall) |
| ab12 T=12 | 1.013 → **1.026** | +66,3 ± 73,4 ms | −155,9 | **paridade dentro do ruído** |

O mecanismo **dispara nos três**; o ganho de wall é **dependente de T/pipeline**
(CPU economizada ÷ threads efetivas): grande em T=1, relevante no CPRR, nulo no
`ab12` (V4 totalmente paralelo, dominado por `position_build`). O líquido
encadeado vs o **original** (`bp_pre_p2 × f1`) é **paridade** — CPRR ≈1.009,
ab12 ≈1.039 (ruído), ab1 ≈1.023 (+2,9%) — ou seja, **recuperou à paridade, não
superou** (medição mesma-sessão `pre_p2` vs f1 não foi feita). Números,
caveats e o rastreamento do resíduo em `../s24/README.md`.

## 3. Contrato final do builder único

`SparseMatrix::build_from_partial_results` (o "v2"), o único caminho de
construção em produção:

- **Entrada order-agnostic**: aceita qualquer intercalação de `(row, col)` de
  qualquer thread; a emissão **fora da faixa da thread** é caso normal, não
  exceção (C21) — os métodos não precisam conhecer invariantes de emissão.
- **Pipeline (instrumentado)**: `V1` histograma por (thread,linha) + detecção
  de `is_set` · `V2` prefixo de `row_ptr_` (histograma → cursores absolutos) ·
  `V3` scatter por stream (ordem = thread, depois emissão) · `V4` **sort por
  linha + coalescência** (paralelo, `schedule(dynamic,256)`; **fast path**
  estrito quando a linha já chega crescente e sem duplicatas) · `V5`
  compactação entre linhas, **serial por desenho** (`mmmove` paralelo é race).
- **Sem** buffer global de triplet, **sem** TBB, **sem** dependência de bind.
- **Semântica de duplicatas**: `is_set` substitui, senão soma; a ordem
  intra-linha com ≥3 duplicatas é indefinida (desvio ≤ 7e-7 relativo) — C19/C20.
- **Gates de correção** (estado final, todos PASS): `test_csr_ops`
  **99 checks (C1–C23)** inclusive `-DNDEBUG` e T=1/2/4/8/12; `PHASE_VALIDATE=1`
  (4/4 `ok`); dump canônico `PHASE_DUMP_DIR` **bit-idêntico** entre f0/f1 (w1/w3);
  `RK_OUT` listas finais **IDENTICAL ×3**; md5 do sweep **5× `b6a4f54e…`** e A9
  **`112fb487…`**; paridade md5 plain vs `-DPHASE_PROFILE`.
- **Knobs**: `PHASE_BUILDER` (referência A/B), `PHASE_ROWSORT={idx,zip}`,
  `PHASE_DUMP_DIR`, `PHASE_VALIDATE`, `PHASE_PROFILE`; comparador
  `Source/bench/dump_cmp.py`; canário `Source/bench/canary.sh`.

## 4. Lições metodológicas (aproveitáveis na tese)

1. **Drift de sessão é estrutural no WSL2 (~15–20%)**, maior que muitos efeitos
   medidos. Protocolo: ABBA intercalado + canário antes/depois de cada ciclo +
   warm-ups e rep0 descartados; ratio e2e de sessão única é **indicativo**.
2. **Canário curto não separa drift de ruído.** `mult w1` (~70 ms) causou H3 em
   4/4 sessões LHRR e **regrediu 3× por cópia de template** sem troca do
   workload. Correção: **fonte única** (`canary.sh`), workload **`w3`** (≥1 s),
   gate mecânico de piso (≥1000 ms) + checksum. `canary_check` só falha em
   piso/checksum; o **drift** é avaliado no agregador (`_p3_diag.py`).
3. **`std::is_sorted` não serve como critério de skip.** `std::sort` é
   instável; aceitar duplicatas adjacentes permitiria permutar chaves iguais e
   mudar a ordem de soma/`is_set` do coalesce. O critério tem de ser **estrito**
   (`!(a[i-1] < a[i])`) e com **prova** de equivalência — guardado por **C23**
   (`[1,3,3,5]`).
4. **Hint de produtor vs verificação própria**: o builder **verifica a própria
   entrada** em vez de confiar que o produtor ordenou. Um `bool presorted` na
   API seria um invariante novo a manter em N call-sites.
5. **Bit-identidade é o gate forte**; o `checksum` do driver é invariante fraco
   (soma `float` em `double`, ordem intra-linha não verificada). Provas de
   conteúdo/ordem: md5 das suítes, dumps canônicos, `RK_OUT`, C18–C23.
6. **Medir com o mecanismo em mente**: fase paralela medida em **CPU** (rusage,
   somada entre threads) — o ganho de wall é `CPU ÷ threads efetivas`. Uma
   economia de 156 ms de CPU a T=12 é ~13 ms de wall; a T=1 é 196 ms de wall.

## 5. Débitos abertos (gate **antes** dos métodos novos)

1. **Recomparação GraphBLAS/MKL no ambiente correto**, bind controlado dos dois
   lados — o motor mudou fundamentalmente (v2 + fast path) desde a última
   comparação válida; "competitivo com SuiteSparse" só se sustenta com medição
   nova.
2. **Estresse do histograma V1** (`T × rows × 4 B`) em N/T **maiores** que a
   ALOI (N=72000) — o único ponto do v2 sem teste de escala.
3. **Consolidação do `update_ranked_lists` triplicado** — antes do método #3.

Resíduos registrados (não bloqueiam, não perseguir agora): ~140–187 ms de
`dREP` em T=1 não explicados pelas fases instrumentadas (V2 fora de
V4/B3/V5; `../s24/README.md`) e `H_build` +11,9% (não ativa o fast path;
provável ruído de sessão, em observação).

