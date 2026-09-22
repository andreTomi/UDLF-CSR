# Etapa B — Fase 3 (`nth_element` sobre pares contíguos): **rejeitada**

Status: **Fase 3 rejeitada; Fase 3b (materialização condicional) também. Estado final
= Fase 1.** A hipótese — "o comparador indireto `acc.val_of(slot)` domina F2_PRUNE e
materializar pares contíguos o elimina" — foi **falsificada por medição**, inclusive no
regime que era o seu melhor caso (w4).

> Este README substitui a análise anterior desta pasta, que era baseada em **1 rep por
> variante** e por isso concluía "regressão" a partir de números com ±20-30% de
> variabilidade (ver "Controle de ruído" abaixo). Os brutos daquela rodada continuam
> aqui como `*_w?_t1.txt`; os da rodada válida estão em `../ab_raw/`.

## O que foi medido

| variante | commit | código |
|---|---|---|
| `base`  | `86898d5` | pré-Fase 1 (triplets globais + sort global + histograma) |
| `f1`    | `0fba99c` | Fase 1 (fragment-CSR por thread), `nth_element` indireto |
| `f13`   | `b3303ec` | Fase 1 + Fase 3: materializa `pairs` em **toda** linha e **sempre** ordena por coluna |
| `f3b`   | `374a278` | materializa `pairs` **só** quando `touched > limit` **e** acumulador Hash; Dense e Hash-sem-poda ficam idênticos à Fase 1 |
| `f1b`   | = `f1` | **controle de ruído**: mesmo binário de `f1`, rodado de novo |
| `fin`   | `4e57526` | estado final: `SparseMatrix.hpp` restaurado de `0fba99c` (kernel = Fase 1) |

Método: cada binário é recompilado do mesmo driver (`ab_build.sh`), mesmos flags,
`taskset -c 0`, `PHASE_MEM=1`; **mediana dos reps 1..n** com rep 0 descartado
(`ab_agg.py`); 5 reps em w1/w2, 3 em w3/w4.

## Resultado (ms, mediana de reps 1..n)

```
wl  var   median_ms   spread%    F1_ACC   F2_PRUN   MP_CALL   BUILD   HWM_MB
w1  base      548.4      30.9     202.5     191.1     548.4   153.6    207.6
w1  f1        395.8      32.8     208.1     174.8     395.7    12.5    143.5
w1  f13       542.0      20.1     229.6     274.2     541.9    12.9    126.4
w1  f3b       414.2      27.8     211.4     183.7     414.1    13.5    126.4
w1  f1b       426.9      36.5     224.4     184.7     426.8    11.1    126.4
w1  fin       373.6       0.5     185.2     168.1     373.6    17.8    143.5
w2  base      595.6      26.9     252.3     179.3     595.5   145.9    190.1
w2  f1        531.2      15.1     266.6     242.2     531.2    13.0    126.2
w2  f13       589.3      27.3     303.4     262.8     589.3    11.5    126.2
w2  f3b       533.5      22.3     256.4     249.6     533.5    14.0    126.2
w2  f1b       571.9      21.5     294.6     245.9     571.9    13.9    126.3
w2  fin       498.5      11.8     243.8     230.7     498.5    15.1    124.2
w3  base    21520.5      20.5   10850.8    8314.3   21518.5  2384.1    654.8
w3  f1      18166.4       5.8    9890.1    7999.3   18163.6   227.7    408.9
w3  f13     19244.0       2.5   10076.2    8944.1   19242.1   199.4    409.3
w3  f3b     21255.5      14.6   11816.6    8956.3   21251.1   464.4    408.9
w3  f1b     19209.1       1.6   10699.3    8266.7   19206.8   228.8    408.9
w4  f1      30493.2       1.1   16911.7   10827.9   30493.2     6.4     99.2
w4  f3b     31835.4       0.4   16592.1   12517.3   31835.3     6.0    110.6
```

Checksums idênticos em todas as variantes: w1 `-503.017268`, w2 `277.889357`,
w3 `8273.428589`, w4 `1795.051013` (nnz 2 500 000 / 2 500 000 / 20 000 000 / 1 000 000).

A rodada `fin` é o **estado final já revertido**: `w1 373,6 ms` / F2 `168,1` e
`w2 498,5 ms` / F2 `230,7`, ambos dentro da banda de `f1` (código idêntico — o diff de
`fin` contra `0fba99c` é vazio), com md5 das suites igual ao baseline a 1/2/4/8/12
threads.

## w4: o workload que fecha a questão

w4 foi adicionado ao driver (`WORKLOADS`) porque é o **único** regime em que os pares
contíguos poderiam pagar: branch **Hash** com `bound_max = 65536`,
`ratio = B.cols/bound = 488` (> `ADAPTIVE_RATIO = 200`, logo Hash) e tabela
`slot_val + slot_col` = **512 KB** (~10× a L1d, ~2× a L2 por core). Com `limit = 50`, o
`nth_element` percorre os 65 536 slots de cada linha: F2 = 10.8 s de 30.5 s (35%).
`rows_a=20000, inner=20000, cols_b=32e6, ra=rb=256`.

## Leitura

1. **Fase 3 regride F2_PRUNE** — a fase que ela existia para melhorar: w1 `274.2` vs
   `174.8`/`184.7` (f1/f1b) = **+48%**; w2 `262.8` vs `242.2`/`245.9` = +8..16%; w3
   `8944` vs `7999`/`8267` = +8..12%. Duas causas, ambas introduzidas pelo diff:
   (a) materializar `pairs` mesmo quando `touched <= limit` — sem poda não há
   `nth_element` a acelerar, é só uma passada a mais; (b) o sort por coluna passou a
   rodar **sempre**, inclusive no Dense, onde `col_of(slot) == slot` já entrega `used`
   em ordem de coluna (era justamente o `if constexpr` que a Fase 1 usava para pular).
   Em w1 (Dense, `touched = 400`, `limit = 50`) a causa (b) responde pela maior parte
   do dano.
2. **Controle de ruído (`f1` vs `f1b`, binário idêntico)**: em w1/w2 o `spread%` é
   15-37% e `F1_ACCUM` — fase que **nenhuma** variante tocou — varia ±20-30% entre
   rodadas de 0.5 s. Conclusão metodológica: **w1/w2 não têm poder de resolução por
   fase**; o julgamento tem de vir de w3/w4 (runs de 18-30 s, `spread%` 0.4-5.8%).
3. **Fase 3b**: no único caminho em que difere de `f1` em w2 (Hash, `touched = 400 >
   limit = 50`) empata (`249.6` vs `242.2`/`245.9`, dentro do ruído de ±8% do
   workload). Em w1/w3 o caminho de código é literalmente o da Fase 1 (o `if constexpr`
   descarta os ramos Hash no instanciamento Dense), logo as diferenças de 5-15% ali
   **são o piso de ruído medido**, não sinal.
4. **w4 falsifica a hipótese no seu melhor caso.** Mesmo com a tabela Hash 10× maior
   que a L1d e o `nth_element` dominando 35% do tempo total: F2 `12517.3` vs `10827.9`
   = **+15.6%** e total **+4.4%** para a versão com pares contíguos, com `spread%` de
   0.4-1.1% e `F1_ACCUM` praticamente igual (`16592` vs `16912`, −1.9%) — o dano está
   inteiramente em F2, não em estado da máquina. O gather
   `slot_val[used[idx]]`/`slot_col[used[idx]]` **não é o gargalo**: copiar 65 536 pares
   por linha custa mais do que o comparador indireto que se pretendia evitar.
5. **Veredito**: a linha dos pares contíguos não tem upside em nenhum dos quatro
   regimes medidos (Dense com bound pequeno e grande, Hash L1-resident, Hash
   L1-busting). `f13` e `f3b` são revertidas; o kernel final é o da Fase 1, sem o
   parâmetro/`std::vector` `pairs`.

## Nota sobre o critério de aceite (md5)

O `checksum` do driver é um invariante **fraco** (soma `float` em ordem de
armazenamento acumulando em `double`, 6 decimais, ordem intra-linha não verificada).
**Retratação**: eu afirmei antes que "o md5 das suítes é forte porque elas imprimem o
conteúdo das linhas" — **falso**. Nenhum dos dois arquivos imprime matriz alguma: o
md5 hasheia apenas títulos de caso, linhas `[note]`/`[FAIL]` e o resumo final, i.e. é
um fingerprint de lista-de-casos + pass/fail. A prova de **valores** vinha do
`same_result`/`to_map` (mapa: insensível a ordem **e** a duplicatas) — e a prova de
**ordem** não existia (o `csr_well_formed` do `test_csr_ops` só era aplicado a saídas
de helpers cujas entradas já eram ordenadas). É por isso que o bug de ordenação do
Dense (este README) passou por todos os critérios de aceite anteriores. A partir de
`6d1c86e` a ordem é verificada por C18 e por asserções `#ifndef NDEBUG` no kernel.

## Reprodução

```bash
bash  Source/bench/ab_build.sh          # base/f1/f13 das respectivas commits em /tmp/inc_*
bash  Source/bench/ab_w4.sh             # f1/f3b (f3b = worktree) com o w4 no driver
bash  Source/bench/ab_run.sh /tmp/ab    # w1/w2/w3: 5 reps, taskset -c 0, PHASE_MEM=1
bash  Source/bench/ab_noise.sh          # controle de ruído (f1b)
python3 Source/bench/ab_agg.py Data/phase_profile/etapa_b/ab_raw
```

Brutos: `Data/phase_profile/etapa_b/ab_raw/*.txt` (19 rodadas) · tabela:
`ab_raw/summary.txt`.

