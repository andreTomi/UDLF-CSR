# S2.4 — regressão S2.3 p2: Fase 0 (diagnóstico ABBA) + Fase 1 (fast path V4)

**Fase 0** = evidência **read-only** (nenhum código de produção alterado;
`mtimes` de `SparseMatrix.hpp` 16:05 e `bench_phase.cpp` 16:15, anteriores aos
scripts): decidir o próximo passo respondendo H1 (work extra do builder), H2
(loop/layout) e H3 (ruído de sessão) **antes** de tocar o código.
**Fase 1** = a mudança já decidida (**fast path do V4**, critério estrito) +
campanha pré/pós na mesma sessão (`bp_fase0` vs `bp_s23`+f1) + bateria de gates.

## Reprodução

```bash
python3 Source/bench/fix_crlf.py _p3_diag.sh
bash /tmp/_p3_diag.sh abcprr        # modos: ab12 | ab1 | abcprr
python3 _p3_diag.py abcprr > Data/phase_profile/etapa_b/s24/agg/agg_abcprr.txt
# tudo de uma vez: bash _p3_run_all.sh    # estado da campanha: bash _check_s24.sh
# Fase 1 (fast path): pre=bp_fase0, new=bp_s23+f1, evidência em raw_<modo>_f1/
PREBIN=/home/andre/udlf_bin/bp_fase0 \
  OUT=$PWD/Data/phase_profile/etapa_b/s24/raw_ab12_f1 bash /tmp/_p3_diag.sh ab12
python3 _p3_diag.py ab12 _f1 > Data/phase_profile/etapa_b/s24/agg/agg_ab12_f1.txt
```

- Braços: `pre = /home/andre/udlf_bin/bp_pre_p2` (commit 1f56845, `-DNDEBUG`) vs
  `new = /home/andre/udlf_bin/bp_s23` (árvore de trabalho, S2.3 p2).
- Protocolo: ALOI N=72000 L=400 K=10 LIMIT=400, `taskset` + `OMP/PHASE_THREADS`
  fixos (T=12 ou T=1), warm-ups descartados, **rep0 de cada bloco descartado**,
  ABBA por ciclo (`pre,new,new,pre` × 3 ciclos = 6 runs = 12 reps usáveis/arm;
  CPRR REPS=5 → 24/arm), canário após cada ciclo (Fase 0: `mult w1 2` inline,
  origem do H3; Fase 1: `Source/bench/canary.sh` — `mult w3 2`, piso ≥1 s +
  checksum).
- Bruto: `s24/raw_<modo>/` (Fase 0) e `s24/raw_<modo>_f1/` (Fase 1) — 12 runs +
  4 canários + 2 warm por modo.
- **Sessões**: `ab12`/`ab1` rodaram 2× — a sessão 1 está arquivada em
  `raw_*_drift1/` + `agg/agg_*_drift1.txt`; `agg_ab12.txt`/`agg_ab1.txt`
  (20:35) são a sessão 2. `abcprr` rodou 1× (canário PASS à primeira).

## Resultado por modo (REP total, rep0 descartado)

| modo/sessão | pool pre | pool new | ratio | ABBA por ciclo (ms) | ABBA médio | canário mid |
|---|---|---|---|---|---|---|
| **abcprr** (1 sessão) | 131.6 (n=24) | 160.4 (n=24) | **1.219** | +20.5 / +36.7 / +26.3 | **+27.9 ± 8.2** | 102,103,99 → **PASS 4.1%** |
| ab1 sessão 1 | 6520.3 (n=12) | 7008.7 (n=12) | 1.075 | +655.8 / +360.0 / +1439† | +818 ± 558† | 73,72,103 → FAIL 43.7%† |
| ab1 sessão 2 | 6332.1 (n=12) | 6751.4 (n=12) | 1.066 | +387.9 / +346.5 / +626† | +453 ± 151† | 67,68,162 → FAIL 141.6%† |
| ab12 sessão 1 | 1987.6 (n=12) | 2015.5 (n=12) | 1.014 | −431 / +340 / +51 | −13 ± 390 | 123,71,72 → FAIL 73.8% |
| ab12 sessão 2 | 1517.5 (n=12) | 1537.0 (n=12) | 1.013 | +59.9 / +110.9 / +76.2 | +82.3 ± 26.0 | 80,66,71 → FAIL 20.3% |

† c3 contaminado nos dois casos (canário saltou para 103 e 162 ms logo após o
bloco); restringindo a c1+c2 (canário estável): **+7.8%** (sessão 1) e
**+5.8%** (sessão 2) — positivo nos dois ciclos, nas duas sessões.
ab12 sessão 1 inutilizável (gradiente 2.3×: canário 166→71, pre c1 2527 → c3
1670); sessão 2 tem 3/3 ciclos positivos (std 26 ms) mas canário FAIL → apenas
indicativo (~+5.4% ABBA; mediana +1.3%). Das 12 células ABBA LHRR, **11
positivas** — a direção é consistente apesar do ruído.

## Decomposição do delta (new − pre)

| modo/sessão | dREP | dBUILD_CALL | dLOOP (est. total/T) | dALLOC |
|---|---|---|---|---|
| abcprr | +28.9 | **+26.3 (91%)** | +0.5 (2%) | ~0 |
| ab1 s1 | +488.4 | **+431.0 (88%)** | +34.9 (7%) | ~0 |
| ab1 s2 | +419.3 | **+395.4 (94%)** | +64.2 (15%) | ~0 |
| ab12 s1 | +27.8 | +22.7 (82%) | −8.2 | ~0 |
| ab12 s2 | +19.5 | +29.7 (152%) | +1.0 (5%) | ~0 |

REST por chamada = dCALL − dLOOP (aloca + build interno): ab1 s2 mult
+157.5 (loop +27.3 → REST +130.2), combine +140.9 (loop +36.9 → REST +104.0);
ab12 s2 combine +25.2 (loop +2.0), mult +13.9 (loop −1.0); CPRR mult
+17.6/+0.3, merge +9.2/+0.2.

DRVAGG (mediana das medianas/run; caveat rep0 incluso) — sobem sempre os
stages de resultado: abcprr mult_A +43.7%, mult_B +26.1%, merge +30.3%;
ab1 s2 mult_S +22.8%, combine_S +24.0%, combine_main +31.9%; ab12 s2
combine_S +15.5%, combine_main +15.9%, mult_Srev +10.1%. `position_build` e
`H_build` planos (+1..5%) — são justamente os que já usavam V2 no pre.

## Achado mecanístico: troca de builder Frag → V2

Contagem de builders por rep (verificada na sessão 2 e em abcprr; bate 1:1 com
o HWM `build_calls`, incrementado só no caminho V2, e com
`V4_ROWSORT calls/rep = builds_V2 × T`):

| modo/braço | `[BuildFrag]` | `[BuildV2]` | build_calls/rep | V4_ROWSORT user_ms |
|---|---|---|---|---|
| LHRR pre | **4** (nnz 5.2M/5.8M/7.3M/8.7M = resultados mult/combine) | 2 (57.6M position, 7.27M H) | 2 | 3269 (s2) |
| LHRR new | 0 | **6** (todos) | 6 | 3661 (Δ+391) |
| CPRR pre | **3** (2.0M/2.3M/2.75M = mult_A/mult_B/merge) | 1 (0.72M w) | 1 | 53 |
| CPRR new | 0 | **4** (todos) | 4 | 265 (Δ+212) |

**S2.3 p2 removeu o builder de fragmentos e passou a construir os resultados de
mult/combine/merge pelo caminho V2 (B3_HIST + V4_ROWSORT + V5_COMPACT).**
Conferência de contas no CPRR (modo com canário PASS): dBUILD +26.3 ≈
ΔV4_user/12 (17.7) + ΔB3_HIST_user/12 (3.5) + ΔB3_PREFIX (2.9) + ΔV5 (≈0) ≈
**+24** — o delta do build é essencialmente o rowsort/hist extra do V2. Em ab1
s2 as fases instrumentadas explicam ~258 dos +395 ms (65%); o restante (~140 ms)
está dentro do V2 fora destas fases — detalhar na Fase 1.

## Veredito e gate

- **H1 CONFIRMADA** — dBUILD/dREP = 91% na única sessão com canário PASS
  (CPRR), 88–94% em ab1, 82–152% em ab12; o achado mecanístico (troca Frag→V2)
  é estrutural e independe de ruído de sessão.
- **H2 REJEITADA** — dLOOP = 2% (CPRR), 7–15% (ab1), ≈0–5% (ab12); loop de
  mult plano no T=1 (s1: +0.0%; s2: +3.0%). Não explica a regressão.
- **H3 confirmada como limitante, não como fonte** — canário FAIL em todas as
  4 sessões LHRR (20–142%): e2e LHRR é indicativo, não conclusivo; mas ruído
  não fabrica a troca de builder nem o viés 11/12 das células ABBA positivas.
- Gates de sanidade: **checksum PASS** e **nnz PASS** em todas as sessões
  (bit-idêntico entre braços; rk md5 já provado no p2).
- **Gate de prosseguimento do plano: PASS → Fase 1 em `SparseMatrix.hpp`**:
  fast path ordenado no builder V2 — quando as entradas de cada linha já saem
  ordenadas (resultados de mult/combine/merge), pular/baratear `V4_ROWSORT`
  (dominante: +212..+391 ms de CPU) sem tocar no resto do pipeline V2 nem no
  position/H (que não regredem).

## Limitações

- `DRVAGG` agrega rep0 (mediana das medianas/run); as fases usadas na
  decomposição são por-rep com rep0 descartado.
- Fases paralelas: `wall_stamp` cobre só a última instância → wall estimado =
  `total_ms/T`; `B3_HIST`/`B5_FILL` são rusage-only (CPU agregado, sem wall).
- HWM `build_calls` é cumulativo (usado como delta entre HWMs consecutivos).
- Pendência opcional: limpar os ~691 MB de rk dumps em
  `/home/andre/udlf_cache/p2_rk/`.

## Fase 1 — resultado (fast path do `V4_ROWSORT`)

**Código** (`Source/src/Core/hpp/SparseMatrix.hpp`): predicado público
`v2_row_is_presorted()` com critério **estrito** `!(cols[i-1] < cols[i])` —
falha em desordem **e** em duplicata adjacente — aplicado antes dos dois modos
de sort (idx/zip). Sem hint de produtor: o builder verifica a própria entrada.
Só pula quando a chave é única, e aí "pular" == "ordenar" **por prova** (chave
única ⇒ única ordem crescente ⇒ mesmo array); linha com duplicata cai sempre no
sort completo = comportamento pré-Fase 1 bit-a-bit (sort instável permutaria
chaves iguais e mudaria a ordem de soma/`is_set` no coalesce). Contrato
(builder 522-536), coalesce, flags, V5 serial e position/H intocados.
**Teste C23** (suíte 96→99 checks): `[1,3,3,5]` (não-decrescente **com**
duplicata) NÃO ativa o fast path + coalescência idêntica à pré-Fase 1 — guarda
explícito contra um refactor futuro trocar o critério por `is_sorted` puro.

**Canário consolidado** (`Source/bench/canary.sh`, fonte única): workload `w3`
fixo, `canary_run`/`canary_check` (piso ≥1000 ms + checksum `8273.428589`).
Call-sites `_p3_diag.sh` e `_p2_profile.sh` corrigidos — o `mult w1` inline era
a **3ª recaída** do mesmo bug de template (H3); `_p3_diag.py` ganhou os gates
de piso e de checksum do canário (PASS nas duas campanhas).

**Braços**: `pre = bp_fase0` (o `bp_s23` congelado antes da edição, md5
`f6fe7d3119ed32c740d28124eb0f0f1a`) vs `new = bp_s23` rebuildado com Fase 1.
Evidência bruta em `raw_<modo>_f1/` + agregação em `agg/agg_<modo>_f1.txt`
(a evidência da Fase 0 ficou preservada em `raw_<modo>/`).

```bash
python3 Source/bench/fix_crlf.py _p3_diag.sh
PREBIN=/home/andre/udlf_bin/bp_fase0 \
  OUT=/mnt/c/.../s24/raw_abcprr_f1 bash /tmp/_p3_diag.sh abcprr   # e ab1
python3 _p3_diag.py abcprr _f1 > Data/phase_profile/etapa_b/s24/agg/agg_abcprr_f1.txt
```

### Números — binários explícitos por coluna

As duas colunas de ratio vêm de **campanhas diferentes** (não de uma sessão
comum), então os binários ficam explícitos:

- **`ratio F0`** = campanha **Fase 0** (`agg/<modo>.txt`): `pre = bp_pre_p2`
  (**original**: fragment-CSR + builder duplo, commit 1f56845) vs
  `new = bp_s23` (**f0** = builder único v2, **sem** fast path). Mede a
  **regressão** causada pela decisão (A).
- **`ratio F1`** = campanha **Fase 1** (`agg/<modo>_f1.txt`): `pre = bp_fase0`
  (= f0 congelado, md5 `f6fe7d31…`) vs `new = bp_s23` (**f1** = v2 **com**
  fast path). Mesma sessão ABBA; isola o **efeito puro do skip do V4**
  (única diferença entre os dois braços é o predicado).
- **`bp_pre_p2` vs f1 na mesma sessão NÃO foi medido** — o líquido contra o
  código original só existe encadeado (cross-sessão, indicativo; ver
  "Resíduo ~35%"). Não confundir as colunas: `0.828`/`0.960` são ganhos
  **sobre o f0**, não sobre o `bp_pre_p2`.

| modo | ratio F0 (pre_p2 vs f0) | ratio F1 (f0 vs f1) | ABBA Fase 1 | dBUILD F0→F1 | ΔV4_ROWSORT user F0→F1 |
|---|---|---|---|---|---|
| **abcprr** T=12 | 1.219 | **0.828** | −16.3 ± 11.6 ms (3/3 neg) | +26.3 → **−10.7** | +212 → **−77.2** (157→80) |
| **ab1** T=1 | 1.066 | **0.960** | −366.2 ± 136.3 ms (3/3 neg) | +395.4 → **−199.1** | +230.3 → **−196.4** (2347→2150) |
| **ab12** T=12 | 1.013 | **1.026** | +66.3 ± 73.4 ms (3/3 pos, c1 outlier) | +29.7 → **+57.3** | +391.3 → **−155.9** (3694→3538) |

- Critérios de sucesso: CPRR `dBUILD` +26,3 → **~0** ✓; ratio 1,219
  **majoritariamente recuperado** (0.828; ver caveat do canário abaixo);
  T=1 ratio 1,066 → 0.960 (recuperado e invertido na sessão); dívida
  `B3_HIST`+`B3_PREFIX` **não se materializou** (−14,0 / −0,1 ms);
  `position_build` **plano** (−0,2%) ✓.
- **`ab12` (T=12, lhrr) NÃO confirma ganho**: `ratio F1 = 1.026`, ABBA
  **+66,3 ± 73,4 ms** (3/3 positivos, mas c1=+149,9 é outlier; c2/c3 =
  +36/+13 ≈ ruído) — **indistinguível de paridade**. O mecanismo **dispara**
  (`V4_ROWSORT` −155,9 ms CPU, −4,2%), mas a T=12 isso equivale a ~13 ms de
  wall (CPU ÷ 12 threads), pequeno demais para mover o end-to-end. É a config
  **menos propensa a mostrar ganho**: já era a menor regressão da Fase 0
  (1.013) e o V4 é totalmente paralelo (dominado por `position_build`).
  Sessão **limpa** (canário drift 3,75% PASS — a melhor de todo o S2), então
  este "sem ganho" é um resultado sólido, não ruído grosso.
- **O ganho é dependente de T/pipeline**: o skip economiza **CPU** no V4
  (abcprr −77, ab1 −196, ab12 −156 ms); o ganho de wall ≈ CPU ÷ threads
  efetivas. T=1 (ab1): CPU ≈ wall → ganho grande (−366 ms, −4,0%); CPRR T=12:
  ganho relevante (−16 ms / −17,2%), parte vinda do `REST` serial dos builds
  pequenos (mult/merge); ab12 T=12: V4 largamente paralelo → ganho no ruído.
- **Caveat (H3)**: deriva mid-sessão do canário = **FAIL 20.55%** (abcprr;
  1 outlier, canary_1=4677 ms vs ~3900 dos demais) e **FAIL 11.93%** (ab1) →
  ratios e2e são INDICATIVOS (mesma limitação das sessões LHRR da Fase 0).
  A direção, porém, não depende do canário: 3/3 ciclos ABBA negativos nos
  dois modos, gates **checksum PASS / nnz PASS / piso PASS / checksum canário
  PASS**, e o ΔV4 é medição direta de CPU da fase tocada.
- **Nota de ambiente (WSL2)**: mesmo com o canário consolidado, o drift
  mid-run foi 20,55% (abcprr) e 11,93% (ab1); `ab12` foi PASS (3,75%). O WSL2
  apresenta **ruído de sessão estrutural de ~15-20%** independente do
  workload → campanhas futuras devem tratar ABBA + evidência triangulada
  (ciclos na mesma direção + fase medida em CPU + gates bit-exatos) como
  padrão, e ratio e2e de sessão única como **indicativo**.
- Fora do alvo: `H_build` **+11,9% (+38 ms)** — H não ativa o fast path
  (esperava-se plano como `position_build`); candidato a ruído de sessão
  (canário FAIL), manter em observação. `mult_S` −11,6%, `mult_Srev` −14,3%,
  `combine_S` −8,7%, `combine_main` +0,8%.
- Por que V4 cai e não vai a zero: o skip elimina o **sort** das linhas que
  chegam estritamente ordenadas (resultados de mult — ordenados desde o fix
  do C18 —, combine, merge), mas o **coalesce O(nnz)**, as fases V1–V3 e as
  linhas com duplicata/desordenada continuam; a atribuição por chamada
  confirma o mecanismo (mult REST −136,4 ms, combine REST −70,6 ms).

### Resíduo ~35% do T=1 (rastreamento — não caiu do relatório)

A Fase 0 fechou com: "em ab1 s2 as fases instrumentadas explicam ~258 dos
+395 ms de dBUILD (65%); o restante (**~140 ms = ~35% do dREP**) está dentro
do V2 fora destas fases — detalhar na Fase 1". Status após o fix:

- **Dentro do build (sessão Fase 1, f1 vs f0)**: dBUILD −199,1 ms com
  `V4_ROWSORT` −196,4 → as fases instrumentadas explicam **~99%** do delta de
  build (resíduo interno ≈ −3 ms, contra 65% na Fase 0). O componente
  positivo não-instrumentado de ~140 ms **não reaparece nesta comparação**.
- **Composição com o pre_p2 (cross-sessão, INDICATIVA)** — líquido encadeado
  (`ratio F0 × ratio F1`), que é o que se pode afirmar como "vs o original":

  | modo | F0 (pre_p2→f0) | F1 (f0→f1) | líquido (pre_p2→f1) | leitura |
  |---|---|---|---|---|
  | abcprr T=12 | 1.219 | 0.828 | **≈1.009** | paridade (encadeamento CPRR é fraco) |
  | ab1 T=1 | 1.066 | 0.960 | **≈1.023** (pools 6332→6520 = +2,9%) | recuperou ~55% do +6,6% |
  | ab12 T=12 | 1.013 | 1.026 | **≈1.039** | paridade dentro do ruído (ABBA +66±73) |

  A âncora `bp_fase0` é estável entre sessões (pool T=1: 6751,4 na Fase 0 s2 vs
  6790,2 hoje, **+0,6%**), o que dá alguma confiança ao encadeamento; ainda
  assim **nenhum modo superou o `bp_pre_p2`** — o resultado é **paridade
  recuperada** (CPRR/ab12) a **~+3%** (ab1). Para afirmar ganho líquido vs o
  original seria preciso medir `bp_pre_p2` vs f1 na **mesma sessão** (não feito).
- **Veredito do resíduo**: magnitude **se manteve** (~140 → ~187 ms), mas a
  **proporção mudou**: era 35% do delta total, hoje é **~100% do que resta**
  da regressão em T=1. Ele agora é o alvo explícito do próximo
  diagnóstico (V2 fora de V4/B3/V5: rastreamento de memória/alloc do V2,
  efeitos de footprint Frag→V2) — registrado aqui para não sumir do
  relatório.

### Gates de regressão (Passo D — todos PASS)

- `run_all.sh`: `ALL TESTS PASSED: 99 checks` (C1–C23) + A1–A9; md5 do
  sweep T=1/2/4/8/12 **5× `b6a4f54ec7dc0e9c9b251254597ea215`**, A9 **4×
  `112fb487…`** (inalterado), instantiation OK, e2e dense==adaptive.
- Variantes `-DNDEBUG`: PASS (99 checks + A-suite). Paridade md5 plain vs
  `-DPHASE_PROFILE`: PASS nas duas suítes (instrumentação output-neutra);
  `bench_phase.cpp` `-fsyntax-only` OK nos dois modos.
- Dump canônico `PHASE_DUMP_DIR` **bp_fase0 vs bp_s23 bit-idêntico** (w1 e
  w3, `ab.bin` + `result.bin`; REP checksums iguais: −503,017268 / 8273,428589).
- `PHASE_VALIDATE=1` smoke (real lhrr): **4× `VALIDATE … ok`, 0 FAIL**.
- `RK_OUT` listas finais (`_fase1_rkcmp.sh`): **IDENTICAL ×3** (lhrr12,
  lhrr1, cprr12), sizes OK (115200000 B); `_p2_cksum.sh`: PASS nos 3 modos.
