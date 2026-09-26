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
| Bit-exatidão | **CK_FINAL único** em P0/P1/P2 sobre `A`+`A2`+`A3`+`A4` (10 runs/ponto, 3 valores de `B`) e sobre `A2`+`A3`+`A4` em P3 (6 runs); `BITFINAL=YES`, `POS_EQUAL=1,000000` e P@1/5/10/20 = 1,000000 contra o dourado nos 30 runs dos pontos sintéticos |
| Gate `F6a` | `BIT=0`, `BEYOND=0`, `GATE_OK=N` em N=5000/10000/13000 (`MAXREL ≤ 5,91e-07`) |
| Knob `B` (batch F5/F7) | **depende de `L`**: `B=256` ótimo em `L=1400` (`B=64`/`512` → +45%); `B=1024` melhor em `L=100`. Critério empírico: scratch `B×N×4 B` deve viver na LLC sem fragmentar o paralelismo |
| Knob `limit` | só `E` (pesquisa); `limit=N` dá `W` fiel com arredondamento dentro dos portões |
| Reproduzir | `g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src Source/bench/bench_bfstree.cpp -o /tmp/bench_bfstree`; `bash Source/bench/run_bfstree_campaign.sh <0..3>`; `python3 Source/bench/bfstree_agg.py` |

`-ffp-contract=off` **não é opcional**: sem ele o GCC forma FMA no dot do F7, a
ordem de arredondamento muda e `BITFINAL` falha — a mesma armadilha registrada no
port do RDPAC.

## 4. Escala: a extrapolação de memória foi **refutada** para a config real

A Etapa 3 validou um modelo estrutural de memória (`after-F5` ≈ 4,23 B por
célula de `N²` + ~6 KB por linha) em dois pontos **de baixa densidade**
(k=12, ρ=0,72) e extrapolei dele **−42%** de memória em N=72.000. A campanha
final mede a grandeza que o modelo não continha: **a densidade depende de `k`,
não de `L`**, e na config real (k=20) ρ(`W`) = **0,99922**.

`W` no port é CSR: `cols` `int32` + valores `float` ≈ **8 B por nnz**, contra
**4 B por célula** da `W` densa do legado. O custo por célula de `N²` é
`8·ρ + estrutura`:

| configuração | ρ medida | RSS medido | B por célula de `N²` |
|---|---|---|---|
| P0 N=5000 k=20 (`A4`) | 0,999995 | 166–178 MB | 6,8–7,3 (legado 8,4 → **−11 a −17%**) |
| P1 N=10000 k=12 (`A4`) | 0,660–0,724 | 462–490 MB | 4,8–5,1 (legado 8,1 → **−37%**) |
| **P3 N=13000 k=20 L=1400 (`A4`)** | **0,99922** | **1,54 GB** | **9,6** (legado ≈ 8,4–8,7 → **paridade a +10%**) |

Consequência honesta: em N=72.000 com k=20, `W` em CSR ≈ 8 B × 5,18×10⁹ =
**41,5 GB** e o total do `A4` ≈ **42,5 GB** — **não** os ~24 GB afirmados no
commit `314c194`, e essencialmente a mesma memória do legado (~43,5 GB). O
**−42% vale apenas no regime ρ ≲ 0,7** (k≤12); na config real o port entrega
**tempo**, não memória. Registro a refutação aqui em vez de emendar o commit.

A alavanca que devolve a memória segue direto desse número: com ρ≈1 os arrays
`cols` são **informação redundante** — `W` deve ser armazenada em **linhas densas
de `float`** (4 B/nnz, sem `cols`), o que dá ≈ **21 GB** em N=72k (**−52% vs
legado**) e ainda torna o gather do `A4` um acesso `row[c]` direto, sem indireção.
Fica como débito #1 (§6), com ganho esperado quantificado.

Tempo projetado para N=72.000, L=1400, k=20 com `A4` (escala `N²·L` nas fases
dominantes, a partir do melhor run de P3): **≈ 22 min**, com F7 ≈ 76% e F5 ≈ 19%
do total. Requisito de RAM da medição real: **≥ 48 GB** (não 32 GB, como se
estimou antes da correção de ρ).

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
3. **Densidade é um número a medir, não assumir.** ρ(`W`) = 0,99922 em k=20 e
   0,66 em k=12 decidem sozinhos se CSR vale a pena; a extrapolação de memória da
   Etapa 3 era correta no regime medido e falsa no regime de produção (§4).
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

## 6. Débitos abertos

1. **`A5` — `W` em linhas densas (`float`, sem `cols`)** quando ρ ≳ 0,7: −52% de
   memória projetada em N=72k (§4) e gather do F7 sem indireção. Maior ganho
   restante, com gate já pronto (`CK_FINAL` do `A4`).
2. **Medição real em N=72.000** em nó com ≥ 48 GB: os comandos estão em §3
   (`run 3` cobre o regime; trocar `N=13000` por `N=72000`).
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
- Brutos desta etapa: `raw/bfstree_campaign_{0,1,2,3}.log`,
  `raw/campaign_all.out`; tabelas consolidadas: `raw/aggregate.md`.
- Legado referenciado: `BFSTree.cpp` / `BFSTree.hpp`; API de acesso por linha:
  `Source/src/Core/hpp/SparseMatrix.hpp` (`for_each_in_row`).
- Commits do arco: `aac7448` (Etapa 2), `314c194` (Etapa 3: `A3`/`F6a`/
  memprobe), `462c207` (driver), `d9bbeef` (Etapa 4: `A4`/`A4b`).


