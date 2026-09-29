# A5 — `W` guardada por valor: implementação, censo e validação bit-exata

O débito 6.1 do `bfstree_close` pedia um portador cuja `W` fosse CSR **por
valor** em vez de densa `N²` (20,7 GB dos ~24,8 GB do `A4` em N=72k) e
declarava o que faltava: *"provar que pular termos ausentes é bit-exato
(`W ≥ 0`, parciais não-negativas)"*. Este dossiê fecha essa prova — **medida,
não deduzida** — em três pontos reais, incluindo os dois datasets que carregam
a motivação da tese (vegfru/sun397), e regista o que o patch **não** entregou.

## 1. O que foi implementado (`Source/bench/bench_bfstree.cpp`, modos `A5`/`A5CENSUS`)

| fase | no A5 |
|---|---|
| F1/F2 (normalização de rank) | **continua densa** — limitação medida, §4(a) |
| F3 (`imgCor`) | idêntica ao A4 (mapas, já esparsa) |
| F4 + F5 | `zero_matrix` + **`build_w_a5`**: cada linha acumula num `RowAcc` por valor **na mesma ordem de visita do `replay_q_into_row` do A2** (espelho comentado, sincronia exigida); células visitadas são emitidas como CSR |
| F6 | `colsum_a5`: início `1.0`, linhas ascendentes por coluna — mesma ordem do `colsum_legacy_exact` (zeros = no-op exatos) |
| F7 | `compute_w_diffusion_a5`: mesmo `vi`/`buf` por posição e mesma acumulação `posM` ascendente do kernel A4; leituras via `w.get()`; **termo pulado somente se `vi·buf == 0.0f`** (no-op exato; um `NaN` nunca cai nesse teste, pois `NaN == 0.0f` é falso) |
| F8/F9 | idênticos ao A4 (já esparsos sobre `rc`) |

**O ponto cego declarado** (por que a algebra não bastava): o denso calcula
`(float)(W[i][c]/acum[c])` para **toda** célula, inclusive as ausentes; se
`acum[c] == 0` o resultado é `0.0/0.0 = NaN` e um CSR que simplesmente não
tem a célula daria `0.0f`. Por construção o kernel A5 reproduz a expressão
(`get()` ausente → `0.0f`, dividido por `acum[c]`), e o censo **conta** as
colunas com `acum[c] == 0` no dado real — a política de skip foi decidida pela
medição, não pelo "deveria funcionar".

O **gate da malha** (`gate_bfstree.sh`) agora roda `ref/A/A2/A4/A5` no pinado
`CK_FINAL=0x7b70dfb313c2b151` (N=800): `BFSTREE GATES: OK`.

## 2. Método da validação

- Driver `Source/bench/run_a5_bitex.sh` (estágios `probe|veg|sun|aloi|all|report`;
  um caso por `.log`; golden `.bin` regenerado pelo `ref` e **gitignored**).
- Mesma postura do gate: `-std=gnu++2b -O2 -ffp-contract=off -fopenmp`,
  12 threads, `OMP_WAIT_POLICY=PASSIVE`.
- Protocolo `real_bitex` da Etapa 1: o **legado (`ref`) grava o golden** na mesma
  entrada real, `A4` (controle) e `A5` leem; asserção `CK_FINAL` idêntico +
  `BITFINAL=YES POS_EQUAL=1.000000`; `A5` roda **duas vezes** (determinismo) e
  `A5CENSUS` compara `A5` contra o denso no mesmo processo.

### Pontos (contrato medido antes, `probe`, todos rc=0)

| ponto | N | L | min_survivors | ρ (`DENSITY`) | `NNZ_W` |
|---|---|---|---|---|---|
| **vegfru** | 13000 | 40 | 46 | 0,110692 | 18706869 |
| **sun397** | 18000 | 25 | 67 | 0,159123 | 51555944 |
| **ALOI** (referência da Etapa 1) | 18000 | 1150 | 1171 | 0,022247 | 7208004 |

`ALOI@18000/L=1150` reproduz `min_survivors=1171` e `NNZ_W=7208004`
(idênticos ao marco `real_bitex` registrado no `bfstree_close` §5) — o
protocolo é o mesmo, agora aplicado fora do ALOI.

## 3. Resultados

### 3.1 Bit-exatidão (o resultado central)

| ponto | golden `ref` | `A4` | `A5` (run 1) | `A5` (run 2) |
|---|---|---|---|---|
| vegfru@13k | `0x8294b8d507e421af` | **igual** `BITFINAL=YES` | **igual** `YES / 1,000000` | **igual** (`CK_W=0x91cad1f2dbd59ea1` idêntico) |
| sun397@18k | `0x960866bac8168a40` | **igual** `BITFINAL=YES` | **igual** `YES / 1,000000` | **igual** (idêntico) |
| ALOI@18k | `0xb803eee6862a0751` | — | **igual** `YES / 1,000000` | — |

**3/3 pontos bit-exatos**, determinístico entre execuções, `POS_EQUAL=1,000000`
em todos (não é "quase igual": é a saída inteira — `P1=P5=P10=P20=1,0`).

### 3.2 Censo A5-vs-denso (`A5CENSUS`, rc=0 nos três = diffs zerados)

| ponto | células | `STORED_DIFF` | `DENSE_NONZERO_MISSING` | `COLSUM_DIFF` | **`COLSUM_ZERO`** | `NAN_DENSE`/`NAN_A5` |
|---|---|---|---|---|---|---|
| vegfru@13k | 169×10⁶ | **0** | **0** | **0** | **0** | **0 / 0** |
| sun397@18k | 324×10⁶ | **0** | **0** | **0** | **0** | **0 / 0** |
| ALOI@18k | 324×10⁶ | **0** | **0** | **0** | **0** | **0 / 0** |

Leitura: (a) todo valor guardado bate **bit a bit** com o denso; (b) nenhuma
célula não-zero do denso sumiu do CSR; (c) o colsum coincide célula a célula;
(d) **`COLSUM_ZERO=0` e `NAN=0` nos três dados** — o caso cego do argumento
algebraico **não ocorre nestes pontos** (medido, não presumido); ele continua
sendo a condição a vigiar em qualquer dado novo. Além disso,
`TOUCHED == NONZERO == NNZ_W` e `ZERO_TOUCHED=0`: o suporte estrutural é
igual ao não-zero (consistente com o gate `TOUCH == NNZ_W` do `rho_datasets`).

### 3.3 Memória: `W` por valor vs densa (e o pico RSS)

| ponto | `W` por valor (medido) | `W` densa `4N²` | redução do termo W | pico `A4` | pico `A5` |
|---|---|---|---|---|---|
| vegfru@13k | 149,7 MB | 676 MB | **4,5×** | 743 MB | 1366 MB |
| sun397@18k | 412,5 MB | 1296 MB | **3,1×** | 1351 MB | 3107 MB |
| ALOI@18k | 57,7 MB | 1296 MB | **22,4×** | — | 2327 MB |

**O pico RSS não caiu: subiu** nos dois pontos com `A4` de controle. Causas
medidas, §4(a).

### 3.4 Tempo (12 threads, ms)

| ponto | modo | F5 | F6 | F7 | total |
|---|---|---|---|---|---|
| vegfru@13k | `A4` | 2896 | 442 | 100 | **6296** |
| | `A5` | 4041 | 14 | 179 | **5455** |
| sun397@18k | `A4` | 5863 | 1015 | 146 | **12045** |
| | `A5` | 8792 | 43 | 176 | **10717** |
| ALOI@18k | legado (`ref`) | 3230 | 11339 | 55842 | **86785** |
| | `A5` | 2802 | 5 | **89398** | **98334** |

`A5_F7_SKIPPED_TERMS` medido: 0,23 M (vegfru), 0,13 M (sun397), **21,7 mil
milhões** (ALOI, 91% dos termos) — o skip funciona, mas §4(b) mostra que o
**tempo** do F7 não é ganho neste kernel.

## 4. O que este patch **não** entregou (dois achados medidos)

### 4(a) O pico RSS não caiu — e a projeção de ~2,5 GB (débito 6.1) não vale para este patch

Motivos medidos (somam o pico):

1. **O scratch denso `N²` de F1/F2 continua alocado** (`leg::matrix` no loader,
   `want_matrix=true` para todo modo ≠ `TOUCHRAW`): 676 MB (vegfru@13k) e
   1296 MB (18k). O A5 o zera e não o usa mais após F1/F2, mas **pico é marca
   d'água** — alocar e não usar depois não reduz nada.
2. **O transiente de emissão do F5 custa 12 B/célula visitada** (vector de
   triplets): 225 MB (vegfru), 618 MB (sun397), 86 MB (ALOI) — **maior que o
   CSR final** (149,7 / 412,5 / 57,7 MB), porque é `12B × ρ·N²` enquanto o CSR
   é `8B × ρ·N²`.

**Derivação do que falta para a projeção ~2,5 GB em N=72k** (economia estimada
sobre 24,8 GB do `A4`):

- **F1/F2 por valor** (o termo que manda): o padrão de escrita é exatamente
  `N·L` pares `(q,img)` ∪ `(img,q)`; a ordem de acumulação por célula é
  `iteração min(r,c)` primeiro, depois a outra — determinística e replicável
  por valor (tabelas por linha com as posições, `O(N·L)` de memória, como o
  próprio débito 6.1 de contabilidade `A4_CSR_NNZ = N·L` já pressupõe). Sem
  isto, o pico continua `4N²` **+** o CSR.
- **Emissão do F5 em streaming** (merge incremental por linha em vez do vector
  global de triplets): some os 12 B/célula de transiente.

Nenhum dos dois foi feito aqui (escopo aprovado = F5 por valor + F7 esparso +
censo). O que **está entregue e medido**: o termo `W` cai 3,1–22,4× e a
exatidão é preservada — a estrutura por valor é a base; faltam as duas peças
de memória acima para o pico cair.

### 4(b) O F7 esparso é mais **lento** no caso de L grande

Medido: `F7 A5` = 179 ms vs `F7 A4` = 100 ms (vegfru, L=40) e 89,4 s vs
55,8 s (ALOI, L=1150, contra o legado denso). Causa: o kernel fiel faz `L`
buscas binárias **por par** (`L² log` sondagens por linha) em vez de iterar a
interseção dos conjuntos armazenados — a afirmação do débito 6.1 ("o F7 passa
a sondar `L·ρ·N` em vez de `L²`") **não é entregue por este kernel**. O desenho
que a entrega (e muda a ordem de nenhuma soma): mapa `col → posição em rk_i`
para a linha `i` + união das colunas armazenadas das duas linhas ordenada
pela posição `p` — custo proporcional à interseção, com os `NaN` forçados
(`acum[c]==0`) incluídos. É o próximo passo de código, não foi medido ainda.

O **F6** é o oposto: 14 ms vs 442 ms (vegfru) e 5 ms vs 11339 ms (ALOI,
2267×) — soma só o que existe. E o total do A5 é **mais rápido que o A4** nos
dois pontos com controle (−14% e −11%) porque o F6 esparsos paga o F7 mais
lento; contra o legado no ALOI o A5 fica +13% (o F7 denso do legado é rápido
em ρ=0,022).

## 5. Débitos e leituras atualizados

1. **F1/F2 por valor + emissão streaming** (§4a) — única coisa que separa este
   patch da projeção ~2,5 GB do débito 6.1.
2. **Kernel F7 por interseção** (§4b) — recupera o ganho de tempo prometido.
3. **`COLSUM_ZERO=0` é propriedade dos dados medidos**, não do algoritmo: em
   qualquer dado novo o censo tem de rodar antes de confiar no skip (é o mesmo
   ceticismo do `TP` e do colsum das Etapas 1 e 3).
4. `A5` entra na **Trilha C** só depois do Passo 0 (numérica do motor sob as
   flags de produção) — registro em `Source/src` segue bloqueado, como decidido.

## 6. Arquivos e reprodução

- Implementação: `Source/bench/bench_bfstree.cpp` (modo `A5`, `A5CENSUS`,
  `namespace a5`, dispatch e forward decls).
- Driver: `Source/bench/run_a5_bitex.sh` — `bash Source/bench/run_a5_bitex.sh
  all` (~15 min) e `… report` (tabela derivada dos brutos).
- Gate na malha: `Source/tests/gate_bfstree.sh` agora inclui `A5`
  (`BFSTREE GATES: OK` com `ref/A/A2/A4/A5`).
- Brutos: `<tag>.log` (CK_FINAL/BITFINAL/A5_*/RSS), `runs.out`, goldens
  `g_*.bin` (**gitignored**, regenerados pelo `ref`).

