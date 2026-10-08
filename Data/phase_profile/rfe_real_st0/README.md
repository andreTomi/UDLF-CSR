# Step 3 — Estágio 0: censo do contrato em dados REAIS (ALOI N=72000 L=400)

Ferramenta: `Source/bench/bench_rfe_census.cpp`.

## Protocolo (executado antes de qualquer medição de tempo)

```
host      : WSL2, 12 threads, 7 GB RAM total / 5 GB available
compiler  : g++ (WSL Ubuntu), -std=gnu++2b -O2 -ffp-contract=off -fopenmp
dataset   : /home/andre/udlf_cache/udlf_rk_aloi_N72000_L400.bin
            115.200.000 B = 28.800.000 int32 = 72000 x 400, row-major, LE
params    : k=10  t=2  pa=0.1
N         : DERIVADO do tamanho do arquivo (nunca passado na linha de comando),
            para que um N errado não desloque o stride e produza lixo plausível
ambiente   : OMP_WAIT_POLICY=PASSIVE ; OMP_NUM_THREADS=variado ; RFE_FULL=1 p/ F6+F7b
```

## Como rodar

```bash
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp \
    -I Source/src -I Source/src/Core/hpp \
    Source/bench/bench_rfe_census.cpp -o /tmp/bench_rfe_census

# censo completo: contrato + rho(Gram), sem materializar o Gram
RFE_FULL=1 /tmp/bench_rfe_census $BIN 400 10 2 0.1

# sem RFE_FULL: so o censo (nao aloca o Gram; ~2s)
     /tmp/bench_rfe_census $BIN 400 10 2 0.1

# teste de viabilidade do Estagio 2 (subamostra por prefixo)
     /tmp/bench_rfe_census $BIN 400 10 2 0.1 0 4000
```

Argumentos: `<bin> <L> <k> <t> <pa> [Nlimit] [rho_N]`.
`Nlimit` limita o censo de `self`; `rho_N` limita a subamostra do `rho(Gram)`.

## Resultados

### A) Censo de `self` (todas as 72000 linhas)

```
RKSELF rows=72000 self_at_0=71448 (99.233%) self_in_L=72000 (100.000%)
      self_in_all=72000 (100.000%) out_of_range_ids=0
```

552 linhas violam a premissa "self na posicao 0" do teorema de `grValue`.
**Nenhuma linha perde self, e nenhum id esta fora do dominio** — o core roda.

### B) Contrato de entrada

```
RKCONTRACT rows=72000 kept=72000 dropped_short=0 dropped_noself=0
```

Zero violacoes em escala real. A subamostra por prefixo e recusada:

```
RKCONTRACT rows=4000 kept=0 dropped_short=4000
RKCONTRACT_REFUSED: 4000/4000 ... expected survivors ~ L*(Nsub/Nfull) = 22.2 of 400
```

Razao aritmetica: filtrar `id < N'` de uma linha com `L` ids de `[0,N)` deixa
~`L*(N'/N)` sobreviventes, logo preservar `L` exige `N' >= N`. **Recusar, nunca
preencher** — o guard e uma asercao, nao um `min()`.

### C) rho(Gram), sem materializar o Gram

```
RKPIPE N=72000 emb_nnz=1957862 mean_row=27.2 sorted_emb_keys=1957862
RKGRAM N=72000 nnz_est=11132644 row_max=750 mean_row=154.6 rho_gram=0.002148
RKMEM  legacy dense = 20.74 GB | CSR gram (unpruned) = 0.089 GB
       | CSR gram (union-pruned to L) = 0.088 GB [prune would SAVE 0.9%]
```

Metodo: indice invertido + array de timestamps (sem sort por linha), logo o censo
roda em N de producao. **Auto-validacao:** `nnz_est` do censo analtico e *exatamente*
o `gram_nnz` que F6 materializou.

### Pipeline completo em N=72000

```
T_f1=1.74s  T_f24=0.12s  T_f5=0.02s  T_f6=3.15s  T_f7b=0.15s   total 5.55s
RKF6  gram_nnz=11132644 gram_zeros=7071700 (63.52% of nnz)
RKF7B ck_rk=0x0db02fefdf6bd62c
```

`ck_rk` identico em 1, 4, 8 e 12 threads.

### Escalonamento

| threads | total | speedup | eficiencia | T_f6 |
|---|---|---|---|---|
| 1 | 26.97 s | 1.00x | 100% | 17.91 s |
| 4 | 11.73 s | 2.30x | 58% | 7.68 s |
| 8 | 8.12 s | 3.32x | 42% | 4.96 s |
| 12 | 5.55 s | 4.86x | 41% | 3.15 s |

**F6 e o gargalo (66% do tempo a 1 thread) e escala pior.** E o preco da estrutura
row-parallel forcada: linhas tem `|inv|` desiguais, logo o balanceamento e ruim por
construcao. Eficiencia cai a ~41% em 12 threads.

## Conclusoes do Estagio 0

1. **A hipotese que eu tinha (rho(Gram) ~ 0.17 -> Gram de 7 GB) estava errada.**
   rho = 0.0021; o Gram CSR ocupa 0.089 GB. A union-prune seria quase um no-op
   (0.9%). A memoria vem da esparsidade natural do `CSRMatrix`, nao de poda.
2. **O legado e inviavel em escala real neste host:** 20.74 GB para uma matriz
   densa contra 5 GB disponiveis. O port CSR e 233x mais barato em memoria.
3. **Bit-exactidade em dado real NAO esta estabelecida e nao pode ser aqui:**
   a subamostra por prefixo viola o contrato em 100% das linhas. Bloqueio medido.
4. **O gargalo e F6, e ele escala pior.** Preco do bit-exato, agora com numero.

---

# Step 3 — Stage 3a: atribuicao de custo do F6 (ALOI real, N=72000)

Pergunta que este estagio responde, e nao e "o F6 e lento" (ja se sabe): **o F6 e
lento por ESTRIcao ou por IMPLEMENTACAO?** Os dois tem remedio oposto.

Instrumento: `RFECore::set_f6_profile(true)`, **off por padrao**, para que todo gate
de paridade rode exatamente o codigo de antes. Driver: `Source/tests/step3a_f6prof.sh`.

## Resultado: 77,3% do F6 e um insertion sort O(k²); a estricao e 19,6%

N=72000, 1 thread, F6 = **16,37 s**:

| fase | s | % do F6 | o que e |
|---|---|---|---|
| `t_sort` | **12,649** | **77,3%** | `RowAcc::finalize_candidates` — insertion sort + dedupe |
| `t_acc` | 3,216 | **19,6%** | pass 2 — **a acumulacao ordenada, a estricao de verdade** |
| `t_disc` | 0,198 | 1,2% | pass 1, descoberta da uniao |
| `t_emit` | 0,105 | 0,6% | emissao dos triplets |
| `t_inv` | 0,068 | — | indice invertido (serial) |
| `t_build` | 0,134 | — | `build_from_partial_results` (serial) |

Trabalho medido: `cand_raw = 59.684.264` (com multiplicidade), `cand_uniq = 11.132.644`,
`inner_iters = 59.684.264`, **raw/uniq = 5,36**, `inv_min = 1`, `inv_max = 103`.

Comparacoes modeladas: insertion sort ~ `sum k²/4 ~ 2,47e10` contra as buscas
binarias `inner_iters·log2(uniq) ~ 4,34e8`. **O sort faz ~57× mais trabalho do que
a fase da qual ele era incidental.** 12,649 s para 2,47e10 shifts da 0,51 ns/shift,
que e o custo esperado de um insertion loop dominado por shifts — o modelo fecha.

## Correcao ao que eu escrevi um estagio atras

Eu atribuí a má escalabilidade do F6 ao "desbalanceamento por construção" e precifiquei
"o preco do bit-exato em ~2,4×". **A causa esta errada.** A escalabilidade por fase
separa os dois efeitos:

| caminho critico (thread max) | 1T | 2T | 4T | 8T | 12T | ef. @12T |
|---|---|---|---|---|---|---|
| `t_sort` (quadratico) | 12,649 | 7,198 | 4,053 | 2,667 | 2,287 | **46%** |
| `t_acc` (ordenado, a parte estrita) | 3,216 | 1,682 | 0,893 | 0,479 | 0,370 | **72%** |

**A acumulacao estrita escala quase duas vezes melhor que o sort.** A ma escalabilidade
do F6 nunca foi o preco da estricao — foi o sort quadratico.

E o motivo e mais agudo que "desbalanceamento". O custo por linha do sort cresce com
`k²`, e `k ∝ |inv[i]|`, que varia **1 -> 103 (103×)**. Elevar ao quadrado espalha o custo
do sort por uma faixa de **~10.000×**, enquanto o custo da acumulacao se espalha pelos
103× originais. **O quadratico amplifica o desbalanceamento.**

O preco do bit-exato, entao, **nao e 2,4×**: sao os 19,6% do F6 que correspondem a
`t_acc` — e essa parte e **irredutivel**, porque dividir o laco de `q` de uma linha
entre threads computa `((a0+a1)+a2) + (a3+a4)` onde o legado computa
`(((a0+a1)+a2)+a3)+a4`, que difere em `float`. Row-parallel e a unica divisao
bit-exata disponivel.

## Como isso muda o plano do 3b

As variantes A/B do 3b (dividir `t` entre threads, bit-exato ou nao) atacam **19,6% do
F6**. Deixam de ser prioridade. A prioridade passa a ser um conserto bit-exato *por
construcao*, porque nao muda nenhuma operacao float nem a ordem dos sobreviventes:

> Trocar o insertion sort por `std::sort` em `finalize_candidates()` e bit-exato de
> forma provavel. Pass 1 so empurra pares `(col, 0.0f)`. `finalize_candidates` soma
> duplicados — e `0.0f + 0.0f + … + 0.0f == 0.0f` exatamente, independente da ordem.
> Logo o valor coalescido e sempre exatamente `0.0f`, e o resultado e o mapa de colunas
> unicas ascendentes com valores `0.0f`, *qualquer que seja o sort que o produza*.
> Estabilidade nao pode importar, porque nao ha nada no valor a preservar.

Previsto: `829·log2(829) ~ 8,2e3` comparacoes contra `829²/4 ~ 1,7e5` da insertion —
reducao de ~21×. E o primeiro item do 3b, com gate em `ck_rk` / `ck_gram` inalterados,
nao em argumento.

## Dois bugs meus durante a medicao, registrados porque foram silenciosos

1. `cand_raw` era lido *depois* de `finalize_candidates()` ja ter reencolhido `cols`,
   entao os dois contadores reportavam a contagem unica e `raw/uniq` saia como
   **1,00** tautologico. Parecia um achado — "a uniao nao tem duplicatas" — e era
   puro erro de instrumentacao. A razao real e 5,36.
2. As porcentagens eram thread-segundos somados sobre tempo de parede, produzindo
   **833%** com 12 threads. O perfil agora reporta soma e maximo contra denominadores
   separados (`soma/(t_total·threads)` = uso de capacidade, `max/t_total` = fracao do
   caminho critico).

## Conclusao do 3a

O F6 e 70% do pipeline em dado real, e o sort sozinho e **~50% do tempo de parede
total do pipeline** (12,649/24,43 = 51,8% a 1T, 4,053/8,26 = 49,1% a 4T,
2,287/4,89 = 46,8% a 12T) — um unico insertion sort O(k²), nao a ordem float que
motivou o port inteiro. A estricao que realmente e exigida custa 19,6% do F6 e e
provadamente irredutivel. O proximo passo correto e um conserto de complexidade de
sort, validado por igualdade de digests — nao um esquema de paralelizacao.

## Execucao

```bash
# 1. build
g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src \
    -I Source/src/Core/hpp Source/bench/bench_rfe_census.cpp -o /tmp/bench_rfe_census

# 2. sweep sequencial de threads (RFE_FULL=1 -> F6+F7b, RFE_F6PROF=1 -> perfil)
bash Source/tests/step3a_f6prof.sh
```

`ck_rk = 0x0db02fefdf6bd62c` identico em T=1,2,4,8,12. O sweep e sequencial de
proposito: rodar em paralelo faria as execucoes disputarem os nucleos e destruir a
propria medicao que se quer atribuir.

## Touch-first de linnaeus (Fase B, 2026-10-07)

`step3e_linnaeus_touch.sh` roda o census atraves de `run()` -- o instrumento
que media a sequencia errada agora se valida contra o gate, no mesmo input:

```bash
python3 Source/tests/_run_gate.py Source/tests/step3e_linnaeus_touch.sh
```

| k | ck_gram | ck_rk | gram_nnz | rho |
|---|---|---|---|---|
| 10 | `0xa584ebbe38e86571` (pin do gate, PASS) | `0x09b624c5caae064c` (pin do gate, PASS) | 2.306.216 | 0,0360 |
| 20 | `0xa77502078055a83d` (NOVO fiel) | `0xcb530cc3a7006366` (E2E fiel, PASS) | 10.123.202 | 0,1582 |

`nnz_est == gram_nnz` nos dois configs. O valor SHORT do sweep (18.733.040,
rho 0,2927) e do estado pre-t-loop e esta superado -- nao serve de expectativa.
Raw: `linnaeus_census.log` (mesmo diretorio).
