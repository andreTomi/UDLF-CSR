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

## Generalização: vegfru / sun397 / cifar10 (rank list de ANN em escala)

Os dois datasets que a tese citava como **"onde o GraphBLAS estourava memória e
o motor original não"** (sun397, vegfru) mais o cifar10, submetidos à mesma
grade. Limite de máquina medido antes de desenhar a grade: os modos com front
(`TOUCH`, `A`) alocam a densa `N²` de `leg::matrix` — 103 / 47 / 14 GB nos N
plenos —, então **em N pleno só `TOUCHRAW` cabe**; o censo de contrato
(rc/min_survivors) é do carregador e sai em qualquer modo. Grade executada:
full-N L=1400 (`TOUCHRAW`), N=24000 `TOUCH`+`A`+`TOUCHRAW` no mesmo (N,L)
(gate + delta medido), N=13000 e N=30000 `TOUCH` (curva ρ(N), L constante por
dataset) + `TOUCHRAW` no 30k (2º ponto do delta). 26 runs nesta campanha,
sequenciais.

### Censo L=1400 em N pleno (o carregador responde)

| ds | N | largura | rc L=1400 | min_survivors | rows_short | self fora pos0 | ρ_raw | suporte |
|---|---|---|---|---|---|---|---|---|
| **vegfru** | 160731 | 2000 | **0** | **2000** | 0 | 43 | **0,020497** | 529537840 |
| **sun397** | 108754 | 2000 | **0** | **2000** | 0 | 0 | **0,053097** | 627999265 |
| **cifar10** | 60000 | 1000 | **5** | **1000** | 60000 | 0 | — | — |
| cifar10 (L=1000) | 60000 | 1000 | 0 | 1000 | 0 | 0 | 0,098992 | 356371324 |

**L=1400 é atingível em vegfru e sun397** (sobreviventes = largura = 2000; ids
todos < N, sem filtro) e **não é em cifar10** — 60000/60000 linhas curtas,
recusa `rc=5` registrada em vez de preenchimento com lixo (o mesmo contrato que
salvou oito runs na Etapa 1). `self_missing=0` nos três; vegfru repete o padrão
do linnaeus com 43 linhas de self fora da posição 0 (medido também pelo awk
independente: 43).

### Gate `TOUCH == NNZ_W` em N=24000 (todos rc=0)

| ds | L | min_surv | `TOUCH_SUPPORT` | `A: NNZ_W` | gap |
|---|---|---|---|---|---|
| vegfru | 40 | 46 | 44818581 | **44818581** | **0** |
| sun397 | 25 | 105 | 79229497 | **79229497** | **0** |
| cifar10 | 120 | 129 | 153850577 | **153850577** | **0** |

Igual aos pares-gate de corel5k/linnaeus e da Etapa 1: suporte estrutural ==
suporte não-zero **exato** em mais três coleções. RSS medido: `A`@24k =
**4,53 GB** (3 runs, ±0,01) ⇒ `A`@30k pediria ~7,1 GB (escala N²) e **não foi
rodado** — limite medido, não estimado; `TOUCH`@30k mediu 3,62 GB.
Os candidatos iniciais de L por fórmula fechada erraram (mins medidos 46/17/127
contra 109/176/160 previstos; médias 555/396/283 contra 162/239/217) — rank
list de ANN não é amostra uniforme e a cauda é pesada dos dois lados; o L final
de cada ponto veio do `min_survivors` medido (registrado no cabeçalho do
script). Em sun397 N=13000 **nenhum L ≥ k existe** (min=17) — impossibilidade
medida, não omissão.

### Curva ρ(N) e o delta RAW−TOUCH (a barra de erro do N pleno)

| ds | L | ρ 13k | ρ 24k | ρ 30k | ρ_raw full-N | δ@24k | δ@30k |
|---|---|---|---|---|---|---|---|
| vegfru | 40 | 0,110692 | 0,077810 | 0,063967 | 0,020497 | −8,70% | −8,49% |
| sun397 | 25 | — (sem L válido) | 0,137551 | 0,119292 | 0,053097 | −5,05% | −4,86% |
| cifar10 | 120 | 0,359718 | 0,267102 | 0,245363 | 0,098992 | −35,29% | −37,49% |

(ρ das colunas 13k/24k/30k é TOUCH pós-F1–F3, L constante por dataset; o do
full-N é `TOUCHRAW` com L=1400/1000 — a única leitura possível lá.) O delta
`RAW/TOUCH−1` é **estável em N** (<1 pp em vegfru/sun397,
~2 pp em cifar10) ⇒ licencia derivar o ρ pós-front em N pleno de primeira
ordem: **ρ ≈ raw/(1−δ̄)** ⇒ vegfru **0,0224** (+9,4%), sun397 **0,0559**
(+5,2%), cifar10 **0,1556** (+57,2% sobre o raw). Ressalvas declaradas: δ foi
medido com L pequeno (40/25/120) e o L do full-N é 1400/1000; a correção é
estimativa de 1ª ordem com dois pontos medidos, não uma medição — o número
medido em N pleno continua sendo o raw, e o pós-front em N pleno é débito.

**O que o guard do carregador ganha com este dado — `L=1400` não é parâmetro
geral.** Em N pleno, `L=1400` exige `largura ≥ 1400`: vegfru/sun397 (2000)
passam, cifar10 (1000) **não pode em nenhum N** — o rc=5 nas 60 000 linhas é o
guard funcionando, não um bug. Em sub-corpus, `min_survivors ≈ largura·(N_sub/
N_total)` com cauda pesada (medido: vegfru 46@13k, sun397 17@13k e 67@18k,
cifar10 127@13k) — ou seja, para coleções de listas estreitas (largura/N ≈ 1–2%)
`L=1400` só existe **perto de N pleno**, e é exatamente lá que nada denso cabe
(47–103 GB). O ALOI é a exceção que originou o parâmetro: largura/N = 10%
(7200/72000), `L=1400` cabe a partir de N≈24k (mínimo 1490, Etapa 1) e em N
pleno sobra folga. **Conclusão registrada**: `L=1400` é o `L` da configuração
de produção ALOI; para qualquer outra coleção o `L` de produção deve ser
derivado do `min_survivors` medido no N alvo — que é justamente o que o
carregador já força com `rc=5`. Não bloqueia nada; entra como dado novo para o
guard, não como mudança de código.

### Curva ρ(N) completa — todos os pontos medidos (não só os extremos)

`TOUCH` = pós-F1–F3 com **L constante por dataset** (a leitura limpa); `RAW` =
`TOUCHRAW` (único modo possível em N pleno). `sup/linha` = suporte médio por
linha, o termo que decide a queda.

| ds | L | ρ @13k | ρ @24k | ρ @30k | ρ full-N (RAW) |
|---|---|---|---|---|---|
| vegfru | 40 | **0,110692** (1439/linha) | **0,077810** (1867) | **0,063967** (1919) | 0,020497 (L=1400; 3294/linha) |
| sun397 | 25 | — (min=17, **sem L≥k**) | **0,137551** (3301) | **0,119292** (3579) | 0,053097 (L=1400; 5774) |
| cifar10 | 120 | **0,359718** (4676) | **0,267102** (6410) | **0,245363** (7361) | 0,098992 (L=1000; 5939) |

Ajuste `suporte ~ N^α` (α < 2 = sub-quadrático ⇒ ρ cai):

| ds | α (13k→24k) | α (24k→30k) | queda de ρ equivalente |
|---|---|---|---|
| vegfru | **1,43** | **1,12** | ~ N^−0,6 a N^−0,9 |
| sun397 | — | **1,36** | ~ N^−0,64 |
| cifar10 | **1,52** | **1,62** | ~ N^−0,4 (a mais lenta) |

**Leitura (o que dá e o que não dá para afirmar):**

1. **Dentro de cada dataset a queda é suave e monotônica** com L constante —
   é efeito estrutural de escala, não degrau nem caso de domínio: o suporte por
   linha cresce muito menos que N (vegfru 1439→1919 para **+131%** de N — a
   expansão de vizinhança satura; ρ = (sup/linha)/N cai por construção).
2. **α não é constante** (vegfru 1,43→1,12; cifar10 1,52→1,62) ⇒ **não existe
   uma lei de potência única**; extrapolação quantitativa para além de ~2× do
   intervalo medido **não está licenciada**. O que está licenciado: a direção
   (queda) e o fato de que o **pior caso de extrapolação é cifar10** (denso e
   com α crescente — cai mais devagar).
3. Os pontos full-N **confirmam a direção** mas não são coláveis à curva
   (modo e L diferentes): com L 10–35× maior, ρ ainda é 2–6× menor que aos
   24k/30k.
4. **Efeito N ≠ efeito manifold**: pontos RAW a N=24k — ALOI (L=1400) 0,0192 <
   vegfru (L=40) 0,0710 < sun397 (L=25) 0,1306 < cifar10 (L=120) 0,1729. O `L`
   difere (cada um com o L do seu gate) e ρ **cresce** com `L` — logo a
   comparação é conservadora **a favor do ALOI**: mesmo com 35× mais `L`, ele é
   3,7× mais esparso que o vegfru. O manifold importa além da escala; a curva
   intra-dataset isola a componente N, e a diferença residual em N casado é a
   componente manifold (o par casado ALOI⇔novos continua indisponível pelo
   contrato, acima).
5. Break-even `ρ≈0,5` do CSR-por-valor (8 B/nnz vs 4 B/célula): os quatro
   grandes ficam **todos abaixo** — o pior pós-front corrigido é 0,156
   (cifar10) ⇒ o termo `W` por valor custa `2ρ` do denso, ou seja ≥3,2× menos
   bytes (ALOI/vegfru full-N: ~20–25× menos).

### Leitura (A5 e a conexão com a tese)

- **A premissa do A5 não generaliza**: em N pleno, ρ_raw é 2,0× (vegfru), 5,1×
  (sun397) e 9,6× (cifar10) o do ALOI 72k/L=1400 (0,010315) — e os valores
  pós-front estimados em N pleno são maiores ainda. O ALOI era o extremo
  esparso, não o caso típico.
- **A conexão da tese fica quantificada**: sun397/vegfru (e cifar10) têm ρ(W)
  de produção muito acima do ALOI — é exatamente o regime em que uma
  representação densa `N²` (o caminho que estourava no GraphBLAS) é inviável
  por construção (47–103 GB só para a matriz), enquanto o custo do port CSR é
  proporcional ao suporte (ρ·N²), que é o número medido aqui. Densidade alta
  explica a pressão de memória; não a cria por si só — mas é o dado que faltava
  para a frase deixar de ser anedota.
- **cifar10 é o caso extremo**: ρ_touch 0,245–0,360 nos pontos medidos (W
  quase 1/3 preenchida em 13k) e o maior viés do proxy raw (−35/−37%).

## Débitos

1. **Pares casados com ALOI**: repetir com `L` que o sub-corpus sustenta
   (`min_survivors` medido: 122 em N=5k, 385 em N=8k — medir a curva
   `min_survivors` vs `L` e escolher o `L` casado, ou casar em N maior).
2. **`TOUCHRAW` em lista ANN**: se o N de produção exigir `TOUCHRAW` nestes
   datasets, medir o delta `TOUCHRAW` vs `TOUCH` no maior N em que os dois
   cabem — não herdar a licença do ALOI.
3. Anomalia ρ(L) não monotônico em linnaeus (item acima).
4. **ρ pós-F1–F3 em N pleno** (vegfru/sun/cifar): exige a densa N² (47–103 GB)
   — host de 128 GB fecha em 1 run por dataset; enquanto isso a estimativa
   derivada (raw corrigido pelo δ de 2 pontos) é o que existe, com as ressalvas.
5. **`NNZ_W` acima de N=24k**: `A`@24k mediu 4,53 GB; escala N² ⇒ 30k pediria
   ~7,1 GB e 60k+ passa de 14 GB. Gate fora do 24k exige mais RAM.
6. **δ medido com L do gate** (40/25/120), não com L=1400/1000: subir o L do
   delta exigiria sub-corpus maior, que por sua vez não sustenta L=1400 (o
   triângulo N² × sobreviventes × 8 GB, medido).

## Arquivos

`probe.out` (contrato), `runs.out` (começo/fim/rc de cada run), `<tag>.log`
(bruto por run: `TOUCH_RHO/TOUCH_SUPPORT/NNZ_W/RSS_KB/min_survivors/…`),
`summary.tsv` (uma linha por run, regenerável), `usage.out`/`usage_expected.txt`
(conferência de argv). Reprodução: `bash Source/bench/rho_datasets.sh <estágio>`
— **sequencial**, nunca em paralelo (RSS/tempo por processo).