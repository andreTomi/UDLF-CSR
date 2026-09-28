# Censo de ρ em três manifolds (`rho_datasets`)

Todo ρ das Etapas 2–4 veio de lista sintética e o único ρ real medido (ALOI)
era diversidade de **escala**, não de manifold. Este censo testa a premissa da
A5 ("a produção real é esparsa") fora de uma única coleção, com **pares
casados no mesmo (N, L, k)** — porque ρ é fracao sobre N² e o suporte de linha
cresce com N: comparar coleções em N diferentes nao mede manifold, mede N.

Driver: `Source/bench/rho_datasets.sh` (`probe | build | newds | aloi_small |
big | summary`). Modos: `TOUCHRAW` (censo sobre as listas cruas, sem alocar
N² — roda no N de produção), `TOUCH` (censo após o re-sort de F1/F2, a ordem
que o F5 vê) e `A` (legado denso, imprime `NNZ_W` medido). 18 runs em
28/09/2026, um processo por run, `OMP_WAIT_POLICY=PASSIVE`, 12 threads,
`k=20 B=256` (17 runs). Contrato de entrada do carregador (rc=4 linha sem self, rc=5
linha com `< L` sobreviventes) registrado como **resultado medido**, não
esperado. Brutos: `probe.out`, `runs.out`, `<tag>.log`, agregado em
`summary.tsv` (regenerável: `bash Source/bench/rho_datasets.sh summary`).

## O probe do contrato de entrada

| ds | N | largura | self na posição 0 | linhas largura ≠ |
|---|---|---|---|---|
| `corel5k/ACC.txt` | 5000 | 5000 | 5000/5000 | 0 |
| `linnaeus/linnaeus5_resnet50.txt` | 8000 | 4000 | 7995/8000 (5 linhas com self fora do 1º token; **self ausente: 0**) | 0 |
| `aloi/rkLists.txt` (prefixo lido) | 72000 | 7200 | — | 1 |

## Resultados (`summary.tsv`)

### Gate do censo: `TOUCH == NNZ_W` — fecha **exato** fora do ALOI

| ponto | `TOUCH_SUPPORT` | `NNZ_W` (modo `A`) | gap |
|---|---|---|---|
| corel5k N=5000 L=5000 | 15052942 | 15052942 | **0** |
| linnaeus N=8000 L=4000 | 19431216 | 19431216 | **0** |

Igual aos dois pares-gate da Etapa 1 (`bfstree_step1/aggregate.md`, gap `+0`).
Suporte estrutural == suporte não-zero se confirma em duas coleções novas.

### rc do carregador: rc=4 nunca ocorreu; rc=5 só no ALOI em N pequeno

| run | rc | causa medida |
|---|---|---|
| corel5k e linnaeus (todos: L=400/4000/5000) | **0** | contrato passa |
| `aloi_5k_400_*` (raw/touch/A) | **5** | `937 rows with < L survivors; min_survivors=122 mean=536.6` (sub-corpus `id<N` de 72k listas) |
| `aloi_8k_400_*` (raw/touch/A) | **5** | `5 rows with < L survivors; min_survivors=385` — ficou a **1 sobrevivente** do corte |
| `aloi_24k_1400_raw`, `aloi_72k_1400_raw` | 0 | contrato passa |

Consequência: **os pares casados aloi⇄corel5k (5k/400) e aloi⇄linnaeus
(8k/400) ficam indisponíveis** — o lado ALOI recusa em vez de preencher com
lixo (é a asserção que salvou oito runs na Etapa 1, agora medida em dois
manifolds novos). A pergunta de manifold com N casado **continua em aberto**;
fecha-se repetindo com um `L` que o sub-corpus ALOI sustenta (medir
`min_survivors` vs `L` antes — ver débitos).

### TOUCHRAW como proxy: a licença **não** transfere para lista ANN ground-truth

`TOUCHRAW` mede sobre as listas cruas; `TOUCH` mede após o re-sort de F1/F2 —
a ordem muda o `top-k` de expansão (`l1`/`l2` do F5) e portanto o suporte.
Delta medido (`TOUCHRAW/TOUCH − 1`):

| ponto | Δ rel. |
|---|---|
| aloi 24000/1400 (Etapa 1, `touchraw_p5` vs `touch_p5`) | **−1,96%** |
| corel5k 5000/400 | **−36,7%** |
| corel5k 5000/5000 | **−37,0%** |
| linnaeus 8000/400 | **−30,4%** |
| linnaeus 8000/4000 | **−24,6%** |

No ALOI e no sintético o delta pequeno (≤2%) foi o que licenciou `TOUCHRAW`
como proxy no N de produção. Nas duas coleções novas — rank lists de ANN
ground-truth, onde a ordem crua não é a ordem do F1/F2 — o erro é de
**25–37%**: `TOUCHRAW` **não** é proxy aceitável nestes dados. O gate
`TOUCHRAW == TOUCH` é refutado aqui (ele nunca foi identidade, era proxy com
erro medido; o erro medido em listas ANN é grande). Usar `TOUCH` onde couber —
cabe: RSS máx. medido 636 MB (linnaeus A) e 396 MB (linnaeus touch).

### ρ medido (a matéria-prima)

| ds | N | L | L/N | modo | ρ | suporte |
|---|---|---|---|---|---|---|
| corel5k | 5000 | 400 | 0,08 | TOUCH | **0,599368** | 14984196 |
| corel5k | 5000 | 5000 | 1,00 | TOUCH | **0,602118** | 15052942 |
| linnaeus | 8000 | 400 | 0,05 | TOUCH | **0,329130** | 21064337 |
| linnaeus | 8000 | 4000 | 0,50 | TOUCH | **0,303613** | 19431216 |
| aloi | 24000 | 1400 | 0,058 | TOUCHRAW | **0,019171** | 11042492 |
| aloi | 72000 | 1400 | 0,019 | TOUCHRAW | **0,010315** | 53472548 |

Reprodutibilidade: os dois pontos ALOI reproduzem **exatamente** os números da
Etapa 1 (`touchraw_p5_real_24k` = 11042492, `touchraw_p4_real_72k` = 53472548).

Leitura com as réguas do próprio script: ρ de corel5k/linnaeus a estas escalas
é **30–60× o do ALOI** — a premissa "produção real é esparsa" vale no ALOI e
**não se generalizou** para estes dois manifolds nos pontos medidos. O que
**não** se afirma: comparação de manifold entre coleções (nenhum par casado
sobreviveu ao contrato, ver acima) e extrapolação para N grande (ρ cai com N;
os pontos de escala são aloi). Anotado também: em linnaeus o ρ `TOUCH` em
L=400 (0,329) é **maior** que em L=4000 (0,304) — não monotônico em `L`,
consistente com o `top-k` de expansão mudar de membro após o re-sort da
truncagem; investigar antes de usar ρ(L) como curva.

## Débitos

1. **Pares casados com ALOI**: repetir com `L` que o sub-corpus sustenta
   (`min_survivors` medido: 122 em N=5k, 385 em N=8k — medir a curva
   `min_survivors` vs `L` e escolher o `L` casado, ou casar em N maior).
2. **`TOUCHRAW` em lista ANN**: se o N de produção exigir `TOUCHRAW` nestes
   datasets, medir o delta `TOUCHRAW` vs `TOUCH` no maior N em que os dois
   cabem — não herdar a licença do ALOI.
3. Anomalia ρ(L) não monotônico em linnaeus (item acima).

## Arquivos

`probe.out` (contrato), `runs.out` (começo/fim/rc de cada run), `<tag>.log`
(bruto por run: `TOUCH_RHO/TOUCH_SUPPORT/NNZ_W/RSS_KB/min_survivors/…`),
`summary.tsv` (uma linha por run, regenerável), `usage.out`/`usage_expected.txt`
(conferência de argv). Reprodução: `bash Source/bench/rho_datasets.sh <estágio>`
— **sequencial**, nunca em paralelo (RSS/tempo por processo).