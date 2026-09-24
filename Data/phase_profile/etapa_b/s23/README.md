# S2.3 passo 1 -- fragment-CSR vs v2 na MESMA base emitida (ALOI N=72000 L=400)

Gate de entrada do S2.3 (doc 13): **medir** a decisao fragment-CSR-vs-v2 na
mesma base emitida, por matriz, ANTES de qualquer remocao de codigo
(`temp_triplets_` / `buildCSRParallel` / `<execution>`). Nenhuma remocao
aconteceu neste passo; a decisao arquitetonica fica para revisao (revisada e
EXECUTADA no S2.3 passo 2 -- ver a ultima secao deste arquivo, com os numeros
reais que corrigem a estimativa de partida).

- Driver unico `/home/andre/udlf_bin/bp_s23` (s22b + hook de medicao S2.3;
  build: `Source/bench/build_s23.sh` = linha do s22b README:118-119 +
  `-DNDEBUG` -- a evidencia de contrato vem da sonda FRAGPROBE, que nao
  depende de assert).
- Harness `Source/bench/s23_run.sh` (resume-avel; skip so se o arquivo
  contiver `DONE,`); agregador `Source/bench/s23_agg.py` -> `aggregate.txt`
  (**GATE: PASS**, exit 0).
- Knob `PHASE_FRAGAB=1` (inerte quando 0/ausente). O hook e um function
  pointer dentro de `CSRMatrix` sob `-DPHASE_PROFILE`; sem a flag o bloco
  nao existe -- suites e producao nao veem nada.
- Capturas: `t{1,12}/raw/` = warm descartado + canario antes/depois + `m1`
  (reps=3; rep0 descartado, politica permanente do S1).

## Protocolo (base identica, amostras intercaladas)

Por build natural do LHRR (seq 1..6 = MM, H, S, Srev, Stot, result; 2 reps
uteis x 3 amostras intra = 6 amostras/builder/matriz):

1. `FRAGPROBE` -- conta o contrato de fragmentos na base VIVA por faixa
   (C.1 linhas na banda do thread, C.2 (row,col) ascendente, C.3 dupes
   adjacentes coalescidos, `is_set`). Contado, nao asserido: sobrevive a
   `-DNDEBUG` e vira evidencia de tabela em vez de abortar a campanha.
2. `FRAGPREP` -- quando o contrato falha, normalizacao COBRIDA (bucket por
   faixa + `stable_sort` (row,col) + soma por grupo == regra V4 do v2,
   SparseMatrix:1084-1094) produz a base do brao frag; `prep_ms` reportado
   aparte. O brao v2 nunca paga prep (contrato order-agnostic: aceita a
   base crua). `FRAGREJECT (C19)` aborta o brao frag se um grupo de dupes
   tocar `is_set=true` (nao ocorre em LHRR: `is_set=0` em toda sonda).
3. `FRAGAB` -- 3 amostras por builder sobre a MESMA base viva (natural
   primeiro, depois frag/v2 alternando; ambos os builders apenas LEEM os
   allocators -- precondicao provada no S2.3).
4. `FRAGABCHECK` -- saida natural vs 1o counterpart: estrutura identica
   (`row_nnz` por linha + sequencia de colunas) e valores <=1e-6 relativo
   (nota de aceitacao do S2).

**Contaminacao de ledger**: com o knob ligado as builds de medicao tambem
gravam linhas `PHASEPROFILE`; esta campanha le SO `FRAG*` + `REP/REPRU/DONE`
(nao agregar o ledger de run com FRAGAB ativo).

## Tabela (mediana de 6; tot/v2 = (frag+prep)/v2, >1 = frag com norma mais lento)

### T=1

| matriz | frag (ms) | prep (ms) | frag_total | v2 (ms) | tot/v2 | modo |
|--------|-----------|-----------|------------|---------|--------|------|
| MM     | 157.6 | 6519.6 | 6677.2 | 2757.6 | **2.42x** | norm |
| H      | 8.5   | 639.9  | 648.5  | 263.4  | **2.46x** | norm |
| S      | 46.4  | 0      | 46.4   | 109.5  | **0.42x** | direct |
| Srev   | 63.7  | 0      | 63.7   | 123.4  | **0.52x** | direct |
| Stot   | 28.6  | 0      | 28.6   | 71.7   | **0.40x** | direct |
| result | 26.0  | 0      | 26.0   | 72.9   | **0.36x** | direct |

### T=12

| matriz | frag (ms) | prep (ms) | frag_total | v2 (ms) | tot/v2 | modo |
|--------|-----------|-----------|------------|---------|--------|------|
| MM     | 79.9  | 6266.3 | 6346.3 | 780.5 | **8.13x** | norm |
| H      | 4.6   | 391.8  | 396.4  | 50.1  | **7.91x** | norm |
| S      | 16.2  | 0      | 16.2   | 24.6  | **0.66x** | direct |
| Srev   | 29.6  | 0      | 29.6   | 29.9  | **0.99x** | direct |
| Stot   | 21.1  | 0      | 21.1   | 21.7  | **0.97x** | direct |
| result | 24.8  | 0      | 24.8   | 17.7  | **1.40x** | direct |

## Pares ABBA por matriz (metodo reproduzivel; classificacoes PROVISORIAS n=6)

Metodo (fixado em `phase_ab.hpp:296-315`): cada build natural do rep emite
6 amostras FRAGAB na ordem k0..k5 -- natural primeiro, depois frag/v2
alternando. O PAR e a dupla de slots ADJACENTES dentro do mesmo rep util:
`par 1=(k0,k1)`, `par 2=(k2,k3)`, `par 3=(k4,k5)` -> 6 pares/matriz/T
(2 reps uteis x 3; rep0 descartado). Slots pares (k0,k2,k4) = builder
NATURAL (frag em seq 3-6; v2 em MM/H), slots impares = counterpart.
`ratio = frag/v2` sempre (independente do slot); `<1` = frag vence o par.
`med_pair` = mediana das 6 razoes pareadas; `ratio_of_medians` =
mediana(frag)/mediana(v2) (a coluna tot/v2 direto da tabela acima);
`agree` = ambos caem do mesmo lado da faixa morta de +/-5% (0.95-1.05 =
paridade). MM/H usam o build frag CRU no par (prep excluido; o prep e
cobrado por rep e so entra no tot/v2 agregado). Toda linha abaixo vem de
`aggregate.txt` (secao PAIRS) e e regeneravel com
`python3 Source/bench/s23_agg.py`.

### T=1 -- pares (frag_ms / v2_ms / ratio / vence)

| matriz | rep | par | frag_ms | v2_ms | ratio | vence |
|--------|-----|-----|---------|-------|-------|-------|
| MM | 1 | 1 | 262.185 | 2778.714 | 0.094 | frag |
| MM | 1 | 2 | 140.445 | 3008.109 | 0.047 | frag |
| MM | 1 | 3 | 140.145 | 2516.087 | 0.056 | frag |
| MM | 2 | 1 | 261.083 | 2796.176 | 0.093 | frag |
| MM | 2 | 2 | 141.103 | 2736.471 | 0.052 | frag |
| MM | 2 | 3 | 174.139 | 2410.165 | 0.072 | frag |
| H | 1 | 1 | 12.591 | 248.772 | 0.051 | frag |
| H | 1 | 2 | 9.329 | 290.576 | 0.032 | frag |
| H | 1 | 3 | 9.644 | 289.320 | 0.033 | frag |
| H | 2 | 1 | 7.677 | 278.086 | 0.028 | frag |
| H | 2 | 2 | 7.507 | 246.334 | 0.030 | frag |
| H | 2 | 3 | 7.727 | 234.934 | 0.033 | frag |
| S | 1 | 1 | 46.248 | 110.732 | 0.418 | frag |
| S | 1 | 2 | 47.441 | 90.168 | 0.526 | frag |
| S | 1 | 3 | 27.718 | 91.236 | 0.304 | frag |
| S | 2 | 1 | 46.600 | 112.560 | 0.414 | frag |
| S | 2 | 2 | 64.495 | 112.674 | 0.572 | frag |
| S | 2 | 3 | 35.251 | 108.211 | 0.326 | frag |
| Srev | 1 | 1 | 68.318 | 180.007 | 0.380 | frag |
| Srev | 1 | 2 | 103.502 | 110.469 | 0.937 | frag |
| Srev | 1 | 3 | 32.793 | 111.769 | 0.293 | frag |
| Srev | 2 | 1 | 59.121 | 139.849 | 0.423 | frag |
| Srev | 2 | 2 | 91.731 | 110.573 | 0.830 | frag |
| Srev | 2 | 3 | 35.389 | 134.944 | 0.262 | frag |
| Stot | 1 | 1 | 21.361 | 76.919 | 0.278 | frag |
| Stot | 1 | 2 | 38.145 | 68.840 | 0.554 | frag |
| Stot | 1 | 3 | 21.046 | 69.627 | 0.302 | frag |
| Stot | 2 | 1 | 34.988 | 94.263 | 0.371 | frag |
| Stot | 2 | 2 | 43.599 | 71.487 | 0.610 | frag |
| Stot | 2 | 3 | 22.267 | 71.938 | 0.310 | frag |
| result | 1 | 1 | 44.375 | 96.045 | 0.462 | frag |
| result | 1 | 2 | 40.900 | 83.756 | 0.488 | frag |
| result | 1 | 3 | 21.409 | 121.779 | 0.176 | frag |
| result | 2 | 1 | 18.276 | 60.712 | 0.301 | frag |
| result | 2 | 2 | 30.567 | 61.471 | 0.497 | frag |
| result | 2 | 3 | 17.884 | 62.045 | 0.288 | frag |

| matriz | W-L frag-v2 | med_pair | ratio_of_medians | agree | nota |
|--------|-------------|----------|------------------|-------|------|
| MM | 6-0 | 0.064 frag | 0.057 frag | sim | cru, prep excl. |
| H | 6-0 | 0.032 frag | 0.032 frag | sim | cru, prep excl. |
| S | 6-0 | 0.416 frag | 0.424 frag | sim | |
| Srev | 6-0 | 0.401 frag | 0.517 frag | sim | |
| Stot | 6-0 | 0.340 frag | 0.399 frag | sim | |
| result | 6-0 | 0.382 frag | 0.356 frag | sim | |

### T=12 -- pares (frag_ms / v2_ms / ratio / vence)

| matriz | rep | par | frag_ms | v2_ms | ratio | vence |
|--------|-----|-----|---------|-------|-------|-------|
| MM | 1 | 1 | 201.065 | 882.819 | 0.228 | frag |
| MM | 1 | 2 | 79.671 | 766.280 | 0.104 | frag |
| MM | 1 | 3 | 80.211 | 591.577 | 0.136 | frag |
| MM | 2 | 1 | 255.179 | 794.726 | 0.321 | frag |
| MM | 2 | 2 | 78.271 | 822.190 | 0.095 | frag |
| MM | 2 | 3 | 78.717 | 611.463 | 0.129 | frag |
| H | 1 | 1 | 4.705 | 66.140 | 0.071 | frag |
| H | 1 | 2 | 4.384 | 53.403 | 0.082 | frag |
| H | 1 | 3 | 5.463 | 42.056 | 0.130 | frag |
| H | 2 | 1 | 4.438 | 46.825 | 0.095 | frag |
| H | 2 | 2 | 5.273 | 53.587 | 0.098 | frag |
| H | 2 | 3 | 4.538 | 41.820 | 0.109 | frag |
| S | 1 | 1 | 25.360 | 44.688 | 0.567 | frag |
| S | 1 | 2 | 34.094 | 23.855 | 1.429 | v2 |
| S | 1 | 3 | 16.023 | 25.448 | 0.630 | frag |
| S | 2 | 1 | 16.321 | 25.273 | 0.646 | frag |
| S | 2 | 2 | 14.337 | 23.948 | 0.599 | frag |
| S | 2 | 3 | 15.959 | 23.561 | 0.677 | frag |
| Srev | 1 | 1 | 42.255 | 53.545 | 0.789 | frag |
| Srev | 1 | 2 | 40.677 | 29.001 | 1.403 | v2 |
| Srev | 1 | 3 | 18.508 | 29.645 | 0.624 | frag |
| Srev | 2 | 1 | 18.252 | 53.403 | 0.342 | frag |
| Srev | 2 | 2 | 41.146 | 28.511 | 1.443 | v2 |
| Srev | 2 | 3 | 17.990 | 30.215 | 0.595 | frag |
| Stot | 1 | 1 | 28.672 | 36.023 | 0.796 | frag |
| Stot | 1 | 2 | 29.069 | 19.466 | 1.493 | v2 |
| Stot | 1 | 3 | 12.614 | 20.556 | 0.614 | frag |
| Stot | 2 | 1 | 14.586 | 33.813 | 0.431 | frag |
| Stot | 2 | 2 | 27.635 | 19.118 | 1.445 | v2 |
| Stot | 2 | 3 | 11.598 | 22.832 | 0.508 | frag |
| result | 1 | 1 | 26.205 | 32.943 | 0.795 | frag |
| result | 1 | 2 | 24.833 | 17.241 | 1.440 | v2 |
| result | 1 | 3 | 11.244 | 17.581 | 0.640 | frag |
| result | 2 | 1 | 25.202 | 32.762 | 0.769 | frag |
| result | 2 | 2 | 24.832 | 17.775 | 1.397 | v2 |
| result | 2 | 3 | 10.915 | 17.607 | 0.620 | frag |

| matriz | W-L frag-v2 | med_pair | ratio_of_medians | agree | nota |
|--------|-------------|----------|------------------|-------|------|
| MM | 6-0 | 0.132 frag | 0.102 frag | sim | cru, prep excl. |
| H | 6-0 | 0.097 frag | 0.092 frag | sim | cru, prep excl. |
| S | 5-1 | 0.638 frag | 0.657 frag | sim | |
| Srev | 4-2 | 0.707 frag | 0.989 paridade | **NAO** | |
| Stot | 4-2 | 0.705 frag | 0.973 paridade | **NAO** | |
| result | 4-2 | 0.782 frag | 1.404 v2 | **NAO** | |

**Leitura dos pares (PROVISORIA -- n=6, bimodal):**

- **T=1 e unanimidade**: frag vence 24/24 pares conformantes (+12/12 em
  MM/H cru) e os dois estimadores concordam em todas as 6 matrizes ->
  vantagem de 1.9-2.8x e real e replicada neste ponto.
- **T=12 nao da consenso**: S e concordante (5-1, ~0.64-0.66 frag);
  Srev/Stot ficam em paridade pela razao de medianas mas 4-2 frag pela
  mediana pareada (agree=NAO); result INVERTE o sinal entre estimadores
  (1.40x v2 vs 0.78 frag, agree=NAO). Frag lidera 17/24 pares
  conformantes, mas o padrao e posicional (proxima linha).
- **Padrao posicional sistematico**: o par 2 (k2,k3) favorece v2 em 7/8
  ocorrencias (Srev/Stot/result nos dois reps + S rep1), enquanto os pares
  1 e 3 favorecem frag em 16/16. Ha deriva intra-rep propria (result T=12:
  frag cai de ~25ms em k0/k2 para ~11ms em k4; v2 esfria de 32.9ms em k1
  para ~17.5ms em k3/k5) -- os dois estimadores pesam esses slots de
  forma diferente, da' a discordanca entre eles.
- **Assimetria do par 1 (cold-start)**: o par 1 poe o build natural EM
  PIPELINE (k0, alocadores acabaram de ser gravados) contra a PRIMEIRA
  execucao do counterpart (k1, fria). Exemplo: em result T=12 rep1 o v2
  frio custa 32.9ms; se estivesse no calor dos pares seguintes (~17.2ms)
  a razao do par 1 seria 1.52 (v2) e nao 0.795 -- contrafactual
  ILUSTRATIVO, nao medicao. O par 1 nao e comparacao em condicoes iguais.
- **Toda classificacao "padrao real" de T=12 acima e PROVISORIA**: com
  n=6, bimodalidade e dois estimadores discordando em 3 das 4
  conformantes, nada aqui vale como peso decisivo. Se a decisao final
  depender de S, result (ou de Srev/Stot sob o pareamento), ela pede
  confirmacao com amostra maior e/ou ordem contra-balanceada (o harness
  sempre emite natural-primeiro; inverter exigiria mudanca nele, fora do
  escopo do passo 1).

## Sondas de contrato (soma de 2 reps uteis)

| seq | matriz | emitted | band_viol (T=1 / T=12) | order_viol | dupes | is_set | verdict |
|-----|--------|---------|------------------------|------------|-------|--------|---------|
| 1 | MM | 115.200.000 | 0 / 105.528.661 | 57.528.138 / 57.527.957 | 144.000 | 0 | needs_norm |
| 2 | H | 14.544.000 | 0 / 0 | 7.061.434 | 184.548 | 0 | needs_norm |
| 3 | S | 14.573.730 | 0 | 0 | 0 | 0 | ok |
| 4 | Srev | 17.370.380 | 0 | 0 | 0 | 0 | ok |
| 5 | Stot | 11.513.318 | 0 | 0 | 0 | 0 | ok |
| 6 | result | 10.497.070 | 0 | 0 | 0 | 0 | ok |

- MM/H vem da base parcial (multiply) e viola C.1/C.2: `band=0` em T=1
  porque 1 thread = 1 faixa cobre tudo; em T=12 a emissao cruza faixas
  (105,5M fora de banda). A norma reduz MM 57,6M -> 40.215.456 (= nnz
  exato do s22b) e H 7,27M -> 1.982.199 (= nnz exato).
- S..result: produtores ja emitem conforme (C.1-C.3 zerados) -> modo direct.

## Checks de equivalencia (24 no total: 6 matrizes x 2 reps x 2 T -- TODOS PASSAM)

- `struct=ok, col_mismatch=0, val_diff=0` em todos os 24;
- `max_rel`: MM/S/Srev/Stot/result = **0.000e+00** (bit-exato frag-vs-v2);
  H = **3.19e-07** (< 1e-6; mesma escala do 3.36e-7 do s22b).
- Suites confirmam o gap documentado em C19 (v2 carrega is_set no scatter;
  frag-CSR ignora) -- daí o rejeitador conservador do norm.

## Drift de canario (REGRESSAO DE TAMANHO documentada; correcao aplicada)

| T | canary_0 | canary_1 | drift |
|---|----------|----------|-------|
| 1 | 64.003 ms | 88.795 ms | **+38.74%** |
| 12 | 83.619 ms | 121.28 ms | **+45.04%** |

- **A amostra REGRESSOU do padrao >=1s do S1-bis**: esta campanha usou
  `mult w1 2` (~64-121 ms por rep), herdado verbatim do template do
  `s22b_run.sh` -- o mesmo problema de "canario curto" que o S1-bis
  identificou e depois corrigiu com o piso de >=1s (o `mult w3` entrou no
  S2.2b por isso). Causa: o harness do s23 foi copiado do s22b sem trocar
  o workload do canario. Amostra de <100ms nao separa deriva de sessao de
  ruido amostral -> drift aparente de +38.7%/+45.0% contra +/-11% do s22b.
- **ABBA mitiga na comparacao RELATIVA**: frag e v2 sao intercalados
  dentro do mesmo `m1` (par = slots adjacentes), entao deriva uniforme
  dentro da rep cancela nas razoes por par. O drift acima vale como bound
  grosseiro de sessao, nao entra como termo nas razoes.
- **Correcao APLICADA neste pacote**: `s23_run.sh` agora usa `mult w3 2`
  (smoke: REP,1=4817 ms >= 1s, checksum 8273.428589 = w3 esperado);
  campanhas futuras ja nascem com o piso S1-bis. A campanha ATUAL nao foi
  re-executada -- os numeros acima permanecem o historico desta rodada.

## Leitura (evidencia; a decisao e FORA deste passo)

- **Bases conformantes (S..result, direct)**: em T=1 frag vence 24/24
  pares e os dois estimadores concordam (0.36-0.52x = 1.9-2.8x mais
  rapido); em T=12 os estimadores DISCORDAM por matriz (S 5-1
  concordante frag; Srev/Stot paridade vs 4-2 frag; result 1.40x v2 vs
  0.78 frag) -- ver a secao de pares acima. Leitura: nao ha vantagem
  CONSISTENTE e decidivel do fragment em T=12 com esta amostra; a
  discordancia e estrutural (posicao do par / cold-start / deriva
  intra-rep), nao ruido puro, e **toda conclusao de T=12 aqui e
  PROVISORIA (n=6)**. Com o contrato ja garantido a maquina de build frag
  e competitiva (nunca perdedora consistente), mas o piso decisorio de
  T=12 exige amostra maior.
- **Bases que exigem norma (MM/H)**: o build frag PURO e 17.5x (T=1:
  2757.6/157.6) e 9.8x (T=12: 780.5/79.9) mais rapido que o v2, mas o
  `prep` single-thread (6.3-6.5 s) inverte tudo: tot/v2 = 2.4-8.1x.
  100% do custo esta na PREPARACAO da base, nao na maquina de build.
- **Por que frag+prep NUNCA vence v2 em MM/H (estrutural, nao
  incidental)**: o `prep` (bucket por faixa + stable sort (row,col) + soma
  por grupo -- SparseMatrix:1084-1094) e EXATAMENTE o trabalho que o v2
  ja faz internamente (V1 histograma, V2 prefix-sum, V3 scatter ordenado):
  para um produtor que nao emite ordenado, alguem precisa normalizar antes
  do fragment-CSR poder atuar, e essa normalizacao E o proprio builder v2.
  Logo `tot/v2 = (frag+prep)/v2` compara "v2 + build fragmentado
  redundante por cima" contra "so v2" -> sempre >1 por CONSTRUCAO, nao por
  acaso de implementacao. Um prep otimo so convergiria assintoticamente
  para o custo do v2, nunca ficaria abaixo dele. A otica correta para
  produtor nao-conformante e: ou o produtor emite conforme (modo direct),
  ou quem normaliza paga o v2 -- nao existe um terceiro caminho barato.
- O dado empurra para: produtores emitem conforme, ou normaliza (e quem
  paga), ou v2 aceita cru -- ESSA e a decisao arquitetonica; **este passo para
  aqui** para revisao humana (plano S2.3).

## Validacao

- `Source/tests/run_all.sh` (protocolo s22b/s2): `ALL TESTS PASSED`
  (multiply_and_prune A1-A9) + `ALL TESTS PASSED: 96 checks, 0 failures`
  (C1-C22); md5 identicos em T=1/2/4/8/12 (5x `f8b6386e...`) e A9 4x
  `112fb487...`; instantiation check OK; e2e sweep dense==adaptive.
- Smoke `PHASE_FRAGAB=1` T=12: EXIT=0 (6 probes / 36 FRAGAB / 6 checks /
  2 prep / 0 rejects) -- os dois segfaults iniciais (scratch sem rows_,
  reuse sem reset()) corrigidos via ctor (rows,cols) + `reset()`.
- `python3 Source/bench/s23_agg.py` -> **GATE: PASS**: ABKNOBS
  builder=v2+fragab=1, pipeline=lhrr, 12 checks/T (estrutura+valores),
  >=4 amostras por builder/matriz, canarios presentes, secao PAIRS
  (6 pares/matriz/T) gerada; exit 0.
- **Paridade md5 com/sem `-DPHASE_PROFILE`** (pre-commit): suites
  compiladas nos DOIS modos -> `test_csr_ops` `f8b6386e...` identico
  plain-vs-flag e `test_multiply_and_prune` `112fb487...` identico
  plain-vs-flag (instrumentacao e output-neutra); `bench_phase.cpp`
  `-fsyntax-only` OK nos dois modos. Re-run integral das suites no estado
  final: SUITE_EXIT=0 (96 checks, md5 5x + 4x identicos, e2e
  dense==adaptive).
- Inercia do knob: `PHASE_FRAGAB` ausente vs `=0` -> mesmo `fragab=0`
  na ABKNOBS (nenhuma derivada de comportamento sem o knob ligado).
- Canario `mult w3` (correcao do harness): REP,1=4817 ms >= 1 s,
  checksum 8273.428589 = valor esperado de w3.

## Fora de escopo / proximo passo

- **Parada planejada**: nenhuma remocao de `temp_triplets_` /
  `buildCSRParallel` / `<execution>` neste passo -- a tabela acima e o
  insumo para a REVISAO da decisao arquitetonica (S2.3 passo 2).
- `prep` e serial (bucket+sort single-thread): e o custo tal como cobrado
  HOJE; uma norma paralela mudaria os numeros de MM/H (nao medido aqui).
- Ledger PHASEPROFILE contaminado enquanto o knob ligado (se acima).

## S2.3 passo 2 -- decisao (A) EXECUTADA (remocao): numeros reais

A revisao humana aprovou a decisao (A) (remocao dos construtores legado +
fragment-CSR; builder unico v2, order-agnostic) e ela foi executada no
S2.3 p2. Os numeros medidos posteriores REFUTAM a estimativa usada para
justificar a decisao:

| cenario | estimativa (justificativa da decisao) | medido |
|---|---|---|
| T=1 LHRR end-to-end | 1-1,5% | **+8,6%** |
| T=12 LHRR end-to-end | 1-1,5% | **+1,3-3,0%** |
| CPRR end-to-end (T=12) | 1-1,5% | **+21,9%** |

- **Procedencia da estimativa**: a faixa "1-1,5%" foi produzida durante a
  revisao humana da decisao (A), fora do repositorio versionado (a partir
  das tabelas de pares do S2.3 passo 1, T=12); ela nao existe em nenhum
  arquivo do repo. Este paragrafo e o registro canonico que a corrige com
  os numeros medidos.
- **Fontes**: T=1 `../s23p2/ab_T1.txt` (ratio=1.086, n=12/braco,
  ABBA-rt); T=12 `../s23p2/ab_T12.txt` (ratio=1.030, n=6/braco, p=0.002)
  + `../s24/agg/agg_ab12.txt` (mediana +1.3% sobre 80 reps/braco; o ABBA
  com pares fixos deu +5.4%, indicativo); CPRR `../s24/agg/agg_abcprr.txt`
  (ratio=1.219, n=24/braco).
- **Qualidade da evidencia**: o CPRR (abcprr) e o UNICO cenario com
  `GATE deriva canary PASS`; os numeros LHRR sao INDICATIVOS (canario
  FAIL em todas as sessoes LHRR brutas, drift 20-142% -- H3 em
  `../s24/README.md`; a causa foi o canario `mult w1` herdado do template,
  corrigido para `mult w3` no harness).
- **Mecanismo** (H1 CONFIRMADA, fase diagnostico S2.4): o delta
  concentra-se em `BUILD_CALL`; a troca Frag->V2 roteou
  mult/combine/merge pelo builder v2, que roda `V4_ROWSORT`
  incondicionalmente sobre uma base que ja chega ordenada. Contas e
  decomposicao em `../s24/README.md`.