# BFSTree — encerramento do port (Etapas 1→5): W esparso, F6 morto, F7 reescrito

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

`-ffp-contract=off` está nos builds por **postura**, não porque sustente a
identidade: medido em §7.2, em `-O2` o `CK_FINAL` é idêntico com e sem a flag
(N=800 e N=4000). O que quebra o bit-a-bit é o conjunto de flags do build do
produto (`-O3 -march=native -mfma -ffast-math`) — a mesma família de armadilha
registrada no port do RDPAC, mas o gatilho aqui é `-O3`/`-ffast-math`, não a
formação de FMA em `-O2`.

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
   `bench_bfstree.cpp`; `BFSTree.cpp` ainda é o legado. Migrar exige (i) o knob de
   `B` por regime de `L` (débito 4), (ii) **a decisão de arredondamento do §7.4** —
   medido aqui, o `-ffp-contract=off` não é o que sustenta o `BITFINAL`; o que o
   rompe são as flags do build do produto, `-O3 -ffast-math` (§7.2) — e (iii) os
   gates do padrão RDPAC (`check_bfstree_tu`, `test_bfstree_ref/parity/corner`).
   Nada disso foi feito neste encerramento: os gates ligados hoje
   (`gate_bfstree.sh`, `make test-bfstree`) validam o **port dentro do bench**, não
   um método registrado na fábrica.
4. **Heurística de `B`**: `B ≈ alvo_de_scratch / (N·4)`, com alvo ~LLC/2 — hoje é
   tabela manual (`256` para `L=1400`, `1024` para `L=100`).
5. `A3a` e `E` permanecem no binário como knobs de pesquisa, com aviso de que
   **não** são bit-exatos (`A3a` por coincidência passa em P2 — não contar com
   isso).

## 7. Encerramento: o que "incluir o método no framework" quer dizer aqui

A régua não está escrita em documento nenhum — está na malha que o arco do RDPAC
deixou. Um método está *incluído* quando tem registro na fábrica
(`Method.cpp` + `MethodRegister<>`), TU check de registro, suite de paridade e de
casos-limite no `Source/tests/run_all.sh`, alvo `make test-<método>`, bench com
tempos por fase e dossiê. Contra essa régua, o BFSTree estava **medido, mas não
incluído**: nada do que a campanha provou era verificável por outra pessoa, porque
nenhum alvo de build construía `test_bfstree_hyp.cpp` (`grep bfstree` em
`Source/Makefile` e `Source/tests/run_all.sh` devolvia vazio até este encerramento).

| critério (legado do arco RDPAC) | BFSTree |
|---|---|
| golden verbatim disponível | ✅ embutido no `bench_bfstree.cpp` (modo `ref`); original congelado na raiz, md5 em §Arquivos |
| bench com fases + driver com knobs + agregador | ✅ §3 e §4 |
| gate de hipótese na malha | ✅ agora, `gate_bfstree.sh [1/3]` |
| gate de contrato de entrada na malha | ✅ agora, `[2/3]` |
| bit-exactness com `CK_FINAL` pinado na malha | ✅ agora, `[3/3]` |
| `make test-<método>` | ✅ agora, `make test-bfstree` |
| método registrado na fábrica + `Validator` + config | ❌ débito 6.3, condicionado a §7.4 |

### 7.1 O que este encerramento ligou

`Source/tests/gate_bfstree.sh`, chamado por `Source/tests/run_all.sh` e por
`make test-bfstree`. **13 s no total, os dois builds dominam; não toca
`Source/src`** — é o fechamento da medição, não a migração.

| gate | asserção | custo medido |
|---|---|---|
| `[1/3]` hipótese F5 = SpGEMM | `RESULT: 45 checks, 0 failures`; md5 da saída (menos a linha 1, que imprime a contagem de threads) idêntico em **1/4/12 threads** | ≈ 5 s (build + 3 execuções) |
| `[2/3]` contrato de entrada do carregador real | `rc=0` em lista válida; **`rc=5`** em linha com `< L` sobreviventes; **`rc=4`** em linha sem `self` — nos modos `A` **e** `A4`, com fixtures de 60 linhas gerados no próprio script e auto-validados | < 1 s de execução |
| `[3/3]` bit-exactness vs golden do legado | `N=800 L=300 k=8 seed=42 B=256 limit=800`: `CK_FINAL=0x7b70dfb313c2b151` e `NNZ_W=553578` idênticos em `ref/A/A2/A4`, `BITFINAL=YES POS_EQUAL=1,000000`; md5 de `CK_W`+`BITFINAL` invariante em **1/2/12 threads** | < 1 s de execução |

Dois desses gates não existiam como asserção antes:

- **`[2/3]` transforma o "contrato de entrada" da §5 em teste.** A recusa `rc=5`
  foi o que impediu oito runs em N=13k/18k reais de rodarem sobre linha curta
  preenchida com lixo; sem as listas do ALOI no host, ela agora é provada com
  fixtures sintéticos de 60 linhas.
- **`[3/3]` é o gate de arredondamento mais barato que existe**: 0,14 s por modo,
  e qualquer mudança no kernel CSR, na ordem de emissão dos tripletes ou no
  arredondamento do dot move o `CK_FINAL` pinado.

O RSS **não** é pinado no gate: em N=800 o path CSR gasta **mais** que o legado
(42 MB vs 10 MB), porque o crossover de pico é `N* ≈ 12·L` (§4) — RSS é
propriedade de campanha (`bfstree_step1/aggregate.md`), não de gate. O gate também
não é vacuo: medido fora dele, o build com as flags do produto produz outro
`CK_FINAL` (§7.2), ou seja, o pin reage.

### 7.2 A numérica: o obstáculo à inclusão é o build, não o port

Mesmo fonte (`bench_bfstree.cpp`), quatro builds, duas escalas:

| build | golden (`ref`) | `A4` | veredito |
|---|---|---|---|
| `-O2 -ffp-contract=off` (postura da campanha) | `0x7b70dfb313c2b151` | idêntico | `BITFINAL=YES` |
| `-O2` **sem** a flag | `0x7b70dfb313c2b151` | idêntico | `BITFINAL=YES` |
| produto (`-O3 -march=native -mavx2 -mfma -ffast-math`, `Source/Makefile:5,7,19`), N=800 | `0x7b70dfb313c2b151` | **`0x0423b12779c26d3d`** | **`BITFINAL=NO`, `POS_EQUAL=0,999983`** |
| produto, N=4000 | **`0xa299c0fa9515e5de`** | **`0xed6d982a1d98511e`** | **`BITFINAL=NO`, `POS_EQUAL=0,999695`** |

**Parâmetros dos dois fixtures** (recuperados por medicao — antes só existiam
na prosa; agora gravados em `Data/phase_profile/numerics_prod/golden.out`):
**N=800 L=300 k=8 seed=42** e **N=4000 L=1400 k=20 seed=42**, `B=256 limit=N`.

A auditoria da numérica do produto (`Data/phase_profile/numerics_prod/README.md`;
string real do Makefile extraída por `make -C Source -s show-flags`, quatro
regimes, mesma fixture e mesmo gate) adiciona **atribuição de causa** à mesma
tabela:

| regime (gate `gate_bfstree.sh` com as flags do regime) | `A4` N=800 | gate |
|---|---|---|
| PROD (produto completo) | `0x0423b12779c26d3d` (NO, 0,999983) | **falha já na hipótese** (`45 checks, 6 failures`) |
| PRODC (produto **+ `-ffp-contract=off`**) | `0x7829b615b6824fb1` (NO, 0,999975) | **falha** no bit-pin — a contração de FMA **não resolve** |
| NFM (produto **−** as 3 flags de fast-math, **+ `-ffp-contract=off`**) | `0x7b70dfb313c2b151` = golden (**YES, 1,000000**) | **`BFSTREE GATES: OK`**, bit-exato também em N=4000 |
| BENCH (`run_all.sh`, `-O2`) | idêntico ao golden | **`BFSTREE GATES: OK`** |

Marcador `hist_*`: **esperado `IGUAL` só no regime PROD**. Os pins históricos
(`0x0423b127…`, `0xa299c0fa…`) foram medidos com a string do produto
*paraphraseada*; o PROD — string real — os reproduz, o que retrovalida aquela
medição. Em NFM/BENCH o `DIFERENTE` do pin de produto é o valor correto (golden
`-O2`), não uma falha.

1. **Em `-O2`, `-ffp-contract=off` é inerte nestes dados** — idêntico com e sem
   ela nas duas escalas. A correção da §3 está feita: a flag continua nos builds
   por postura idêntica ao RDPAC e como proteção contra o GCC formar FMA no dot do
   F7 em outra versão/tuning, mas não é ela que sustenta o `BITFINAL=YES`.
2. **Sob as flags do produto, quem quebra é o port**: em N=800 o golden denso do
   legado **não se move**, mas o `A4` CSR muda (`BITFINAL=NO`, 1,7×10⁻⁵ das
   células). Em N=4000 o `-O3 -ffast-math` já move o golden também, e os dois
   divergem entre si (3,05×10⁻⁴ das células).
3. Consequência direta para o débito 6.3: levar o `A4` para `Source/src` sem
   mexer no build do produto significa **shippar um método que não reproduz o
   `CK_FINAL`** contra o qual a campanha inteira foi jurada. Não é defeito do
   port — é incompatibilidade de contrato de arredondamento com `-ffast-math`.
4. **A culpa é do `-ffast-math`, não da contração de FMA** (medido com o regime
   PRODC): desligar só a contração faz a hipótese voltar a 45/45, mas o port
   continua não bit-exato (`POS_EQUAL=0,999975`). A reassociação de fast-math
   quebra o port sozinha.
5. **O regime barato existe e está medido (NFM)** — mantém `-O3 -march=native
   -DNDEBUG` e remove só as três flags de fast-math (com `-ffp-contract=off`):
   gate completo **`BFSTREE GATES: OK`** e bit-exatidão em N=800 **e** N=4000,
   a **−0,2%** de PROD no caso sintético de tempo (§7.4). Os quatro regimes,
   com determinismo verificado (`A4_repeticao_md5_unico=1` em todos), estão no
   dossiê `numerics_prod/`.

### 7.3 O que fica bloqueado neste host (fronteira, não falha)

- **Golden em N=24k real**: os portadores batem `CK_FINAL` entre si, mas o golden
  do legado pede as duas densas `N²` (~4,8 GB) — este host de 8 GB não dá sem
  swap. Uma máquina de **32 GB+** fecha isto e a medição em N=72k de uma vez
  (`A4` cabe em ~32 GB, `A5` projetado em ~3 GB).
- **`L=1400` em dados reais**: o censo (§5) mostra que o `L` de produção não é
  atingível em N=18k (mínimo de sobreviventes 1171 < 1400) — por isso o
  `real_bitex` fechou em `L=1150`. `L=1400` real só fecha em N ≳ 24k, ou seja,
  depende do item acima.
- **`A5` (`W` por valor)**: gate pronto (`CK_FINAL` do `A4`), falta a prova de que
  pular termos ausentes é bit-exato.
- **A fronteira do instrumento de valores** (auditoria `numerics_prod`): o
  `nnz=2500000` é invariante nos 4 regimes × 6 casos e as duas suites têm md5
  idêntico ao registrado em `s2_close`; o `checksum`, porém, diverge no PROD
  **só no caso `dense`** (1·10⁻⁶ a 1,3·10⁻⁵) — e ele é soma serial em double,
  o mesmo instrumento que o `-ffast-math` reassocia. Com este instrumento dá
  para afirmar "estrutura idêntica, aritmética reassociada no PROD"; separar
  ULP de reassociação *dentro* do checksum está fora do que a medição cobre.

### 7.4 A decisão de inclusão, quando ela for tomada

Pré-condição (a única de verdade): escolher o regime de arredondamento do produto.

1. **Alvo próprio** — compilar o método com `-ffp-contract=off -fno-fast-math`
   num alvo de build dedicado, e registrar a divergência medida. Preserva o
   contrato de identidade, mas o binário do produto deixa de ser bit-a-bit com o
   atual (os 99 checks históricos do `run_all.sh` são do regime atual).
2. **Contrato de tolerância** — o produto mantém `-ffast-math` e o `BFSTree`
   declara tolerância (com os números da tabela acima como testemunha); o
   `BITFINAL` passa a ser propriedade do regime de teste.

**Custo medido da opção (1)** (`numerics_prod/README.md` Q3): o regime NFM —
tudo do build do produto mantido, menos as três flags de fast-math, mais
`-ffp-contract=off` — **passa o gate completo e é bit-exato em N=800 e N=4000**
por **−0,2%** no tempo (46695,63 vs 46807,34 ms, n=3 intercalados, dentro do
ruído) ⇒ a opção (1) é **gratuita nesta medição**. O caso real (ALOI, n=2)
**não** é utilizável como delta (spread até +44% por drift de sessão WSL2) e
fica registrado como limitação — não existe "ganho de 16%" a reivindicar.

Recomendação: **(1)**, porque `-ffast-math` já torna duvidoso um contrato herdado
do RDPAC ("`0/0` sem guarda = NaN verbatim", Etapa 3) por causa de
`-ffinite-math-only` — o problema é pré-existente, e o BFSTree só o torna visível.

Depois disso, o caminho é o do RDPAC, item por item: `Source/src/Methods/hpp/BFSTree.hpp`
header-only + `MethodRegister<BFSTree> signInMethod("BFSTree")` (padrão
`Method.cpp:11,23`) + `Validator`/config para o knob `B` (débito 6.4) +
`check_bfstree_tu.cpp` + `test_bfstree_ref/parity/corner` (golden verbatim no
padrão `test_rdpac_ref.cpp`, cobrindo o que o censo mapeou: `self` na posição 0 em
só 71.448/72k linhas, recorte `id<N`, recusa de linha curta, ties, `pin`, `k=1`,
`L ≥ 2N`) + md5 estrito 1/2/4/8/12 sem tocar os md5 históricos + ASan/UBSan +
bench sob `OMP_WAIT_POLICY=PASSIVE` (a causa diagnosticada no RDPAC vale igual
para o F7 em batch).

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
- **Procedência do golden** (medida em 27/09/2026). Os `.cpp/.hpp` da raiz estão
  **fora** do versionamento; o que está no repositório são as cópias *verbatim*
  embutidas em `bench_bfstree.cpp` e `test_bfstree_hyp.cpp`, e estes md5 são o laço
  verificável com o upstream: `BFSTree.cpp` `66ca00e255143101408fc3dc25d36742`
  (912 linhas), `BFSTree.hpp` `914f456f591780158eda51702767c240` (100),
  `RDPAC.cpp` `6f6ea70ffe180c2ea5f1f2d297aafe0e` (499), `RDPAC.hpp`
  `ade9d201eab922cf7351cc754b1bff03` (107). Commitar esses arquivos (ex.:
  `reference/legacy/`) é o passo que falta para a campanha ser reproduzível a
  partir do clone — registrado aqui, não feito, porque a política de onde o legado
  mora é do arcabouço, não desta campanha.
- Gates deste encerramento: `Source/tests/gate_bfstree.sh` (chamado por
  `Source/tests/run_all.sh` e por `make test-bfstree`); protocolo reutilizável do
  arco: `Source/bench/README.md`.
- Commits do arco: `aac7448` (Etapa 2), `314c194` (Etapa 3: `A3`/`F6a`/
  memprobe), `462c207` (driver), `d9bbeef` (Etapa 4: `A4`/`A4b`).


