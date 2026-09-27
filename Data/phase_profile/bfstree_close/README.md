# BFSTree — encerramento do port (Etapas 1→4): W esparso, F6 morto, F7 reescrito

Consolidação do port do método BFSTree (difusão com matriz de similaridade `W`)
para o motor CSR do repositório. A restrição que organizou tudo foi **bit-exatidão
com o legado célula a célula**: sem ela, nenhuma das otimizações abaixo seria
aceitável, e nenhuma delas teria sido barata de validar. O detalhe de cada etapa
fica em `Source/bench/bench_bfstree.cpp` (modos) e nos logs de `raw/`; aqui só a
decisão, o mecanismo, o contrato final, a escala e as lições.

| etapa | artefato | o que fechou |
|---|---|---|
| **1** | modo `A` + `ref` (`Source/bench/bench_bfstree.cpp`) | port **verbatim** do legado F1–F9 com fases cronometradas, densidade medida e checksum de `W`/final → dourado bit-exato e linha de base de memória |
| **2** | modos `A2`, `E`, `Eattr` + `run_bfstree_campaign.sh` | `A2` = **exact-blocked** (índice invertido `O(N·k²)` + replay por linha em scratch `B×N`, ordem de acumulação idêntica): **1,5–1,9×** já nesta etapa; `E` (SpGEMM puro) medido e **documentado como pesquisa** (`limit=k` dá 1,2–2,2× com P@20 = 0,62–0,81 ⇒ reprovado); `Eattr` atribui célula a célula as fontes de erro do `W` |
| **3** | modos `A3`, `A3a`, gate `F6a`, `memprobe` | **F6 eliminado**: soma por coluna paralela **exata** + divisão **fundida** na leitura do F7 → `T_F6` 2,10 s → 0,23 s (P1) sem tocar em um bit; gate `F6a` (colsum algébrico `O(N·k²)`) **BEYOND=0** em N=5k/10k/13k |
| **4** | modos `A4`, `A4b` + `bfstree_agg.py` | **F7 reescrito por localidade, bit-exato** (hoisting da linha `i` + gather de coluna crescente em buffer posicional): **−20% em F7** na config real; `A4b` (W normalizado) **rejeitado** por medição |

## 1. A decisão: `A4` como portador de produção

`A4` = `A2` (F5 bloqueado exato) + colsum paralelo exato com divisão fundida
(F6 morto) + F7 reescrito por localidade. É **bit-exato por construção**, não por
amostragem: cada soma mantém a ordem original e cada estreitamento é o mesmo
`float(double/·)` do legado.

| ponto | dourado `A` | `A2`(melhor B) | `A3` | **`A4`** | memória `A` → `A4` |
|---|---|---|---|---|---|
| P0 N=5000 L=100 k=20 (ρ=1,00) | 5,70 s | 3,59 s (1024) | 3,32 s | **3,34 s** | 201 → 178 MB (**−11%**) |
| P1 N=10000 L=100 k=12 (ρ=0,72) | 7,87 s | 4,43 s (4096) | 2,47 s | **2,36 s (3,33×)** | 776 → 490 MB (**−37%**) |
| P2 N=5000 L=100 k=10 (ρ=0,99) | 1,99 s | 1,04 s (256) | 0,63 s | **0,62 s (3,20×)** | 198 → 167 MB (**−16%**) |
| **P3 N=13000 L=1400 k=20 (ρ=0,9992 — config real)** | inviável | 60,76 s | 57,30 s | **50,32 s** | 1,54 GB (legado ≈ paridade, ver §4) |

ρ = densidade medida de `W`. Tempo = parede, 12 threads WSL2,
`OMP_WAIT_POLICY=PASSIVE`; dentro de um ponto os modos rodam costas-com-costas,
então os deltas **intra-ponto** são os números utilizáveis (§5.5).

`A3` continua válido quando `L` é pequeno e simplicidade importa: em P0
`A3` ≈ `A4`; o ganho de `A4` é específico do regime `L`-dominado (P3, F7 = 66%
do total). `A3a`/`E` ficam como knobs de pesquisa, fora do caminho de produção.

## 2. O mecanismo das três eliminações

**(a) F6 — a fase que não precisava existir.** O legado materializa `W` densa,
depois roda uma passada de `N²` divisões pela soma de cada coluna (`acum[i]`).
Duas algebras distintas moravam ali:

- *colsum*: `acum[i] = Σ_j W[j][i]`. No port esparso esse colsum pode ser
  **algébrico** em `O(N·k²)` (a soma de uma coluna é contável a partir das listas
  `rk`). O gate `F6a` provou isso célula a célula — `BEYOND=0`, `MAXREL ≤
  5,9e-07` em N=5k/10k/13k — mas **a ordem de soma muda**, e `A3a` (que usa o
  colsum algébrico) não é bit-exato: `CK_FINAL` difere do dourado em P0/P1 e em
  P2 *coincide por acaso*. Ruído que só aparece em algumas configs é exatamente
  o que um gate de qualidade não pega; o algébrico ficou restrito a validação.
- *divisão*: `W[j][i] /= acum[i]` é coluna-a-coluna, e o **F7 já lê `W` por
  coluna** nas posições de interesse — a divisão vive na leitura do F7. `A3`
  mantém o colsum **exato** (paralelo, banda `O(N²)`, barato) e **não
  materializa `W` normalizada**: a passada inteira de `N²` divisões desaparece
  (`T_F6`: 2,10 s → 0,23 s em P1; 3,80 s → 0,44 s em P3).

**(b) F7 — localidade sem tocar na aritmética.** Em `L=1400` o F7 é 66–71% do
total e é *banda/latência-bound*, não de álgebra. `A4` faz duas mudanças
isomorfas ao legado em resultado **e** em ordem de soma:

1. **hoisting da linha `i`**: `W[i][rk_i[m]] / acum[·]` não depende de `posJ` —
   era recalculado `L` vezes por linha dentro do loop duplo, passa a ser
   calculado uma vez por linha (`L` em vez de `L²`);
2. **gather em ordem crescente de coluna**: a linha `j` é lida nas colunas
   `rk_i[]` ordenadas por endereço (prefetch/TLB favoráveis) e os valores são
   dispersos num buffer **na ordem posicional original**, onde o dot acumula em
   `posM` ascendente — a mesma sequência de parcelas do legado.

O preço é um `std::sort` de `L` inteiros por linha de `i` (amortizado em `L²`
leituras). Em `L=100` isso deixa `A4` ≈ `A3` ou marginalmente pior (P0: 3,39 vs
3,32 s); em `L=1400` vale **−20% de F7** (P3: 39,8 → 33,2 s; pareado costas-com-
costas em N=13k: 55,6 → 44,7 s).

**(c) `A4b` — a hipótese errada, medida.** Como o F7 tem `N²·L` divisões
escalares, a hipótese natural era removê-las materializando `W` normalizada num
passe paralelo. Feito (`A4b`): `T_F7` **piorou** para 65 s em N=18k (vs 52 s do
`A4`) — o passe extra expulsou `W` da cache antes do F7 e as divisões não eram o
gargalo. Registrado como refutação: **não otimizar opcodes de um loop bound por
memória** (mesmo tipo de erro de estimativa que o S2.3 cometeu com "1–1,5%").

**(d) `E` — por que SpGEMM puro não serve aqui.** Com `limit=k` (guardar só `k`
entradas por linha de `W`) o `E` é 1,24–2,16× mais rápido que o legado, mas
`Eattr` mostra o preço célula a célula: com `limit=N`, **0,00%** das células
ultrapassam os portões de tolerância e **0,00%** são espúrias (o `W` do port é
fiel — só arredonda diferente); com `limit=k`, **61–95% das células são podadas**
e P@20 desaba para 0,62–0,81. Em `k=20`, ρ(`W`) = **0,99922**: não há esparsidade
para explorar **dentro de `W`** — o ganho do port vem do **caminho de acesso**
(F5/F6/F7/F8), não de representar `W` como esparso.

## 3. Contrato final e gates

| item | estado |
|---|---|
| Bit-exatidão | **CK_FINAL único** em P0/P1/P2 sobre `A`+`A2`+`A3`+`A4` (10 runs/ponto, 3 valores de `B`) e sobre `A2`+`A3`+`A4` em P3 (6 runs); `BITFINAL=YES`, `POS_EQUAL=1,000000` e P@1/5/10/20 = 1,000000 contra o dourado nos 30 runs dos pontos sintéticos — e agora também sobre **estrutura real**: `BITFINAL=YES` de `A`, `A4` e `A2` contra o golden do legado em N=18.000 (ALOI, `L=1150`, §5) |
| Gate `F6a` | `BIT=0`, `BEYOND=0`, `GATE_OK=N` em N=5000/10000/13000 (`MAXREL ≤ 5,91e-07`) |
| Knob `B` (batch F5/F7) | **depende de `L`**: `B=256` ótimo em `L=1400` (`B=64`/`512` → +45%); `B=1024` melhor em `L=100`. Critério empírico: scratch `B×N×4 B` deve viver na LLC sem fragmentar o paralelismo |
| Knob `limit` | só `E` (pesquisa); `limit=N` dá `W` fiel com arredondamento dentro dos portões |
| Reproduzir | `g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src Source/bench/bench_bfstree.cpp -o /tmp/bench_bfstree`; `bash Source/bench/run_bfstree_campaign.sh <0..3>`; `python3 Source/bench/bfstree_agg.py` |

`-ffp-contract=off` **não é opcional**: sem ele o GCC forma FMA no dot do F7, a
ordem de arredondamento muda e `BITFINAL` falha — a mesma armadilha registrada no
port do RDPAC.

## 4. Escala: a refutação do −42% foi **re-refutada** — o erro era a ρ sintética

Esta seção já afirmou que a extrapolação de −42% estava refutada porque, na config
de produção, ρ(`W`) = 0,99922. **O número estava certo e o rótulo errado**:
0,99922 é a densidade das **listas sintéticas** rodadas na config de produção
(`L=1400, k=20`), não das listas do ALOI. Medida em dados reais, com censo
estrutural que não materializa `W` (modos `TOUCH`/`TOUCHRAW` de
`Source/bench/bench_bfstree.cpp`, driver `run_bfstree_step1.sh`, agregador
`bfstree_step1_agg.py`), a densidade é **50 a 100 vezes menor**:

| dados (config real `L=1400, k=20`) | ρ(`W`) pós-front | ρ(`W`) cru | `W` nnz em N² | \|g_inv\| médio |
|---|---|---|---|---|
| sintético N=13.000 | **0,999220** | 0,998722 | 1,69×10⁸ / 1,69×10⁸ | 359 (máx 491) |
| **ALOI N=24.000** | **0,019554** | 0,019171 | 1,13×10⁷ / 5,76×10⁸ | 64 (máx 183) |
| **ALOI N=72.000** | — | **0,010315** | 5,35×10⁷ / 5,18×10⁹ | 73 (máx 362) |

O gate do censo passou: onde o `NNZ_W` medido é comparável (sem poda por `limit`),
o censo bate **dígito por dígito** — `TOUCH_SUPPORT` 168.868.239 = `NNZ_W` em
N=13.000 sintético, e 11.263.319 = `NNZ_W` medido pelo `A4` em N=24.000 real. As
listas reais têm vizinhanças top-k fortemente sobrepostas (`l1∪l2` colapsa de 381
para ~64 distintos), então 98% do suporte de `W` simplesmente não existe. O ρ≈1 do
sintético era artefato de listas independentes — e foi ele que gerou a refutação.

O segundo achado, este **independente de ρ** e que decide a memória, está nos
brutos do `A4`: `A4_CSR_NNZ` = 18.200.000 em N=13.000 e 33.600.000 em N=24.000 —
**exatamente `N·L` nos dois, sintético e real**. O `rc` da F7 só recebe `L`
entradas por linha; a segunda matriz `n²` do legado aloja ~`L/N` de células úteis
(2% em N=72.000) e é quase inteira desperdício estrutural. O que o CSR devolve em
troca é medido: buffer de tripletos a 12 B por emissão (`A4_TRIP_BYTES`/`A4_TRIPS`
= 12 exato), `rc` em CSR a 8 B por entrada (`A4_CSR_BYTES`/`A4_CSR_NNZ` = 8 exato)
e um pico de ~3× o CSR durante o build.

**Memória medida, costa-com-costa, mesmo ponto** (N=13.000, `L=1400`, sintético,
`BFSTREE_MEMTRACE=1`, um processo por variante):

| variante | RSS após F5 | após F7 | pico | `T_total` | `T_F7` |
|---|---|---|---|---|---|
| legado `A` | 0,808 GB (4,53 B/cél) | 1,442 GB (8,53 B/cél) | 1,442 GB | 126,0 s | 96,1 s |
| `A4` | 0,853 GB | 1,485 GB | **1,617 GB (9,57 B/cél)** | **47,5 s** | **29,9 s** |

O salto F5→F7 do legado é 660 MB = **3,91 B por célula**: a segunda densa, medida
em vez de suposta. Em N=13.000 o `A4` é **+12% em memória** e **2,65× mais rápido**
— a trilha CSR (0,218 GB de tripletos + 0,450 GB de pico no build + 0,146 GB de
`rc`) tem a mesma ordem de grandeza dos 0,66 GB economizados. Daí um crossover que
a §4 antiga não tinha: com ~45 B pagos por entrada de `N·L` e 4 B economizados por
célula de `N²`, `N* ≈ 45·L/4 ≈ 16.000`. Abaixo de `N*` o `A4` é **pior** em
memória; acima, melhora. Em N=24.000 real o `A4` mede **4,03 GB** decompostos em
2,54 GB (W densa + rk + imgCor) + 0,41 GB (tripletos) + 1,08 GB (CSR e build); o
legado no mesmo ponto exige as duas densas (4,61 GB) + os mesmos auxiliares, ≈ 4,8
GB, **fora do orçamento deste host** (~5,9 GB livres): o par direto em escala real
sai em N=18.000, logo abaixo.

A predição do crossover saiu medida no estágio `cross` (sintético ρ≈1, que é o pior
caso porque o `rc` em CSR é proporcional a ρ; `L=1400`; um processo por variante):

| N | pico legado `A` | pico `A4` | razão `A4/A` | `T_total` legado → `A4` |
|---|---|---|---|---|
| 13.000 | 1,442 GB | 1,617 GB | 1,122 | 126,0 s → 47,5 s |
| 18.000 | 2,715 GB | 2,607 GB | **0,960** | 217,9 s → 79,5 s |
| 24.000 | não medido (as duas densas somam 4,6 GB dos ~5,9 GB livres do host) | 4,033 GB | — | — |

Interpolando os dois pontos medidos, **`N* ≈ 16.800 ≈ 12·L`** — o modelo acima dava
16.000. Em N=18.000 o `A4` já é **mais leve e 2,7× mais rápido**, e fecha o mesmo
ponto com `CK_W` (`0xffa7fad35de48dc6`) e `CK_FINAL` (`0xaa71d87a3df024dd`)
idênticos aos do legado: terceira confirmação de equivalência por checksum, agora
em N=18.000. Repare que o pico do `A4` não é o build do CSR (2,42 GB em N=18.000) e
sim o fim da execução (`RSS_KB` final 2,607 GB) — os tripletos são liberados, mas
F8/F9 voltam a crescer; quem dimensiona RAM deve olhar o fim, não o meio.

**Bit-exatidão sobre estrutura real** (estágio `real_bitex`, `N=18.000, L=1150,
k=20` — o `L` máximo que o contrato de entrada permite nesse `N`, §5 item 7; ρ
medido 2,22%, `NNZ_W` 7.208.004). O `mode=ref` roda o legado denso sobre as listas
do ALOI e grava as listas finais; os portadores comparam contra esse arquivo:

| run | modo | pico | `T_total` | `CK_FINAL` | `BITFINAL` |
|---|---|---|---|---|---|
| `ref_p8_real_18k` | legado (golden) | 2,697 GB | 103,8 s | `0xb803eee6862a0751` | golden |
| `a_leg_p8_real_18k` | `A` | 2,697 GB | 108,4 s | idêntico | **YES** |
| `a4_p8_real_18k` | `A4` | **2,385 GB** | **29,2 s** | idêntico | **YES** |
| `a2_p8_real_18k` | `A2` | 2,385 GB | 32,1 s | idêntico | **YES** |

É a primeira vez que a equivalência bit-a-bit sai sobre a correlação de posto real
-- as Etapas 1-4 só haviam medido `BITFINAL` com listas sintéticas -- e ela vale para
os três portadores: `POS_EQUAL = P@1 = P@5 = P@10 = P@20 = 1,000000` nos três, e
`A4_CSR_NNZ` = 20.700.000 = `N·L` exato outra vez. No mesmo ponto o `A4` é **−11,6%
de pico** (razão 0,884, tabela T4) e **3,6× mais rápido** que o golden, quase tudo na
F7 (71,6 s → 15,9 s) e na F8 (11,3 s → 0,44 s). Repare que o `A` denso portado custou
108,4 s contra 103,8 s do legado: o port do caminho denso não acrescenta custo, o
ganho inteiro vem do caminho esparso. A razão 0,884 contra a 0,960 do sintético no
mesmo `N` é o ρ agindo onde o modelo diz (o `rc` em CSR é ∝ ρ; tripletos e a densa
não são), e é por isso que em dados reais o `A4` vira vantajoso ~2.000 amostras antes.
Ressalva de leitura: a coluna "B por entrada de `N·L`" da contabilidade do agregador
dá **47,5 B** neste ponto e 46,8 B aos 13.000 -- o caminho CSR custa o mesmo nos dois
pontos; o que muda é o valor da matriz economizada, que é ∝ `N²`. E o ponto é
`L=1150`, não o `L=1400` de produção, pelo contrato de entrada do §5 item 7.

Projeção para `N=72.000, L=1400, k=20` com todos os coeficientes medidos acima:
legado ≈ **41,9 GB** (8 B×N² + rk); `A4` ≈ **24,8 GB** (W densa 20,7 GB + rk 0,4 +
tripletos 12·N·L = 1,21 + pico de build ~2,4) → **−41%**, que é o número do commit
`314c194`. A refutação anterior estava errada por dois motivos independentes: usou
ρ sintética e atribuiu ao `A4` uma `W` em CSR que ele não tem (`W` é densa em
todos os portadores; o CSR é o `rc`). RAM necessária: **~32 GB**, não 48 GB. Tempo:
o `A4` real em N=24.000 fecha em **54 s** (F7 31, F9 4,9, F6 3,1, F5 3,1, F1F2 3,5,
F3 2,1); escalando F5/F3/F6 por N² e F7/F9 por N dá **≈ 5 min** em N=72.000 — não
os ~22 min projetados antes, que supunham F7 ∝ N²·L e ρ≈1.

O ρ real de 1–2% abre o ganho que a §4 antiga procurava no lugar errado: `W` é
que é 98% zero, e é ela que ainda custa 20,7 GB. Guardar `W` **por valor** (CSR com
`cols`, 8 B/nnz) custa 5,35×10⁷×8 = **0,43 GB** em N=72.000, e o gather do F7 deixa
de ler `L²` densos por consulta para caminhar linhas CSR com um marcador do
conjunto de colunas da consulta: `L × ρ·N` = 1400×740 = 1,04×10⁶ sondas por
consulta contra 1,96×10⁶ do denso — **menos** trabalho, não mais. Pico projetado
**~2,5 GB (−94% vs legado)**. O argumento de bit-exatidão é fechável: `W ≥ 0`
(o censo estrutural e o `NNZ_W` medidos coincidem, logo não há zeros estruturais
por cancelamento), então somar termo ausente é somar `0,0f` a parcial não-negativa,
que é exato — a verificar por `CK_FINAL` contra o `A4`.

## 5. Lições metodológicas (aproveitáveis na tese)

1. **Verificar a decomposição algébrica de uma fase antes de aceitá-la como
   densa.** F6 (100% das `N²` divisões) era redundante com uma leitura que o F7
   já fazia. O custo de descobrir foi uma hora de análise; o de implementar, um
   `if` no caminho de leitura.
2. **Bit-exatidão como gate barateia a otimização.** Com ordem de acumulação e
   narrowing congelados, cada reescrita virou uma questão de *schedule* de
   memória, validável por um `CK_FINAL` de 64 bits em segundos. Um gate de
   qualidade (`POS`, P@20) exigiria dataset, protocolo e tempo por iteração — e
   aceitaria o `E` com P@20=0,81, que é exatamente o que não se quer.
3. **Densidade é um número a medir, e no dado certo.** ρ(`W`) = 0,99922 é a
   densidade das listas *sintéticas* na config de produção; no ALOI é 0,01–0,02
   (§4). A mesma proxy sintética produziu duas conclusões erradas em direções
   opostas (refutar o −42%, depois propor `W` densa sem `cols`). O que tornou a
   medida real possível foi um censo que não aloca `N²`: contar suporte por
   inversão de índice custa `O(N·|inv|·k²)` e ~400 MB de `rk`, não 20 GB.
4. **O knob que se suspeita não é o que domina.** `PARAM_BFSTREE_L=1400` não
   entra no F5 (tamanho `(k-1)²`), e sim em F7/F9/F3 — o gargalo real (66–71%)
   só apareceu porque as fases eram cronometradas individualmente desde a Etapa 1.
5. **Drift de sessão no WSL2 é maior que o efeito medido.** O mesmo código de F5
   mediu 8,1 s e 12,3 s na mesma campanha. Protocolo: medir modos comparáveis
   costas-com-costas dentro do mesmo ponto (é por isso que o driver intercala
   `A2`/`A3`/`A4` por `B` em vez de agrupá-los), e tratar números absolutos
   cross-sessão como indicativos.
6. **Testar a hipótese antes de otimizar.** `A4b` (remover divisões) piorou o F7
   em 25%: loop bound por memória não responde a redução de opcodes. A
   reescrita de localidade (`A4`) é que pagou.

7. **Contrato de entrada se mede, não se assume**: o censo `rk_self_census.awk`
   acha `self` em 100% das 72k linhas mas **na posição 0 em só 71.448** — o pinning
   do carregador não é decorativo. E recortar `id < N` das listas deixa ~`N/10`
   sobreviventes por linha, e o mínimo é o que vale, não a média: em N=13.000 o
   carregador recusa (`rows_short=9567`, mínimo 881) e em N=18.000 também recusa
   (`rows_short=503`, mínimo 1171) em vez de encher linha com lixo. `L=1400` sobre
   o subconjunto real só fecha em `N≈24.000` (mínimo 1490); a bit-exatidão real em
   N=18.000 roda com `L=1150`. Foi esse guard que impediu oito runs de produzirem
   golden inutilizável.
8. **Não se edita um driver com um lote em andamento**: dois lotes deste passo
   morreram com `line 134: n: command not found` e `syntax error near unexpected
   token ';;'` porque o arquivo foi salvo enquanto o `bash` ainda o lia (bash lê
   scripts incrementalmente, e a reimpressão do editor desloca o offset). Lote em
   curso = arquivo congelado; editar cópia em `/tmp`, ou esperar o `_DONE`.

## 6. Débitos abertos

1. **`A5` — `W` guardada por valor (CSR com `cols`), não densa.** Em dados reais
   98% de `W` é zero (§4), e `W` densa é o único termo `N²` que sobra nos
   portadores: 20,7 GB dos ~24,8 GB do `A4` em N=72k. Guardada por valor custa
   0,43 GB, e o F7 passa a sondar `L×ρ·N` entradas em vez de `L²`. Pico projetado
   **~2,5 GB**. Gate pronto: `CK_FINAL` do `A4`; falta provar que pular termos
   ausentes é bit-exato (`W ≥ 0`, parciais não-negativas).
2. **Medição real em N=72.000**: cabe em **~32 GB** com `A4` (não 48 GB) e em ~3 GB
   com o `A5`. A bit-exatidão sobre estrutura real já saiu (`real_bitex`:
   `BITFINAL=YES` de `A`, `A4` e `A2` contra o golden do legado em N=18.000, §5), mas
   em N=24.000 real os portadores só bateram `CK_FINAL` entre si -- o golden daquele
   ponto pede as duas densas do legado (~4,8 GB), que este host de 8 GB não dá sem
   swap. Levar o `real_bitex` a uma máquina de 32 GB+ fecha os dois de uma vez.
3. **Integração no produto**: os portadores `A2/A3/A4` vivem só no
   `bench_bfstree.cpp`; `BFSTree.cpp` ainda é o legado. Migrar exige o knob de
   `B` por regime de `L` e o `-ffp-contract=off` no alvo de build correspondente.
4. **Heurística de `B`**: `B ≈ alvo_de_scratch / (N·4)`, com alvo ~LLC/2 — hoje é
   tabela manual (`256` para `L=1400`, `1024` para `L=100`).
5. `A3a` e `E` permanecem no binário como knobs de pesquisa, com aviso de que
   **não** são bit-exatos (`A3a` por coincidência passa em P2 — não contar com
   isso).

## Arquivos

- Bench/modos: `Source/bench/bench_bfstree.cpp`; driver:
  `Source/bench/run_bfstree_campaign.sh`; agregador:
  `Source/bench/bfstree_agg.py`.
- Passo 1 (dados reais, censo estrutural e memória): driver
  `Source/bench/run_bfstree_step1.sh` (estágios `touch_synth`, `real_rho`,
  `leg_p3`, `cross`, `real_sparse`, `real_bitex`, `real_a4`), agregador
  `Source/bench/bfstree_step1_agg.py`, brutos e `aggregate.md` em
  `Data/phase_profile/bfstree_step1/`, censo de contrato `rk_self_census.awk`.
- Brutos desta etapa: `raw/bfstree_campaign_{0,1,2,3}.log`,
  `raw/campaign_all.out`; tabelas consolidadas: `raw/aggregate.md`.
- Legado referenciado: `BFSTree.cpp` / `BFSTree.hpp`; API de acesso por linha:
  `Source/src/Core/hpp/SparseMatrix.hpp` (`for_each_in_row`).
- Commits do arco: `aac7448` (Etapa 2), `314c194` (Etapa 3: `A3`/`F6a`/
  memprobe), `462c207` (driver), `d9bbeef` (Etapa 4: `A4`/`A4b`).


