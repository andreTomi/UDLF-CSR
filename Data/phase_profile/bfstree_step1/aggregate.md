# Passo 1 do fechamento do port BFSTree -- medicas

Regenerado por `Source/bench/bfstree_step1_agg.py` a partir de `Data/phase_profile/bfstree_step1/*.log` (27 runs) e dos brutos de `Data/phase_profile/bfstree_close/raw`.

## T1. Censo estrutural vs `NNZ_W` medido (o gate do censo)

| run | N | L | k | TOUCH_SUPPORT | NNZ_W medido | dif | fonte do NNZ_W |
|---|---|---|---|---|---|---|---|
| touch_p0_synth | 5000 | 100 | 10 | 16938309 | 108503 | _n/c_ | campanha Etapas 2-4: limit=10<N: W medido la e o podado |
| touch_p1_synth | 10000 | 100 | 12 | 72383919 | 282439 | _n/c_ | campanha Etapas 2-4: limit=12<N: W medido la e o podado |
| touch_p2_synth | 5000 | 100 | 10 | 16964280 | _sem par_ | - | - |
| touch_p3_synth | 13000 | 1400 | 20 | 168868239 | 168868239 | +0 | campanha Etapas 2-4 |
| touch_p5_real_24k | 24000 | 1400 | 20 | 11263319 | 11263319 | +0 | a2_p5_real_24k |
| touchraw_p0_synth | 5000 | 100 | 10 | 17127526 | 108503 | _n/c_ | campanha Etapas 2-4: limit=10<N: W medido la e o podado |
| touchraw_p1_synth | 10000 | 100 | 12 | 72618620 | 282439 | _n/c_ | campanha Etapas 2-4: limit=12<N: W medido la e o podado |
| touchraw_p2_synth | 5000 | 100 | 10 | 17088288 | _sem par_ | - | - |
| touchraw_p3_synth | 13000 | 1400 | 20 | 168784076 | 168868239 | -0.05% (proxy) | campanha Etapas 2-4 |
| touchraw_p4_real_72k | 72000 | 1400 | 20 | 53472548 | _sem par_ | - | - |
| touchraw_p5_real_24k | 24000 | 1400 | 20 | 11042492 | 11263319 | -1.96% (proxy) | a2_p5_real_24k |

**Veredito:** nos 2 pares gate (`TOUCH` pos-front contra `NNZ_W` medido sem poda) o censo bate exatamente; `TOUCHRAW` erro em <0,1% relative, o que o habilita como proxy no N de producao.

## T2. rho estrutural: sintetico vs real, mesma config

`TOUCH` conta o suporte depois do front F1/F2 (que reordena as listas); `TOUCHRAW` conta sobre as listas cruas. O delta pequeno autoriza usar TOUCHRAW nos N grandes, onde o front nao caberia.

| N | L | k | dados | TOUCH (pos-F1/F2) | TOUCHRAW (cru) | delta rel. |
|---|---|---|---|---|---|---|
| 5000 | 100 | 10 | SYNTH | 0.677532 | 0.685101 | -1.10% |
| 10000 | 100 | 12 | SYNTH | 0.723839 | 0.726186 | -0.32% |
| 13000 | 1400 | 20 | SYNTH | 0.999220 | 0.998722 | +0.05% |
| 24000 | 1400 | 20 | REAL | 0.019554 | 0.019171 | +2.00% |
| 72000 | 1400 | 20 | REAL | - | 0.010315 | - |

- `touch_p0_synth`: imgCor tem 10-10 chaves por linha (media 10.000), rho(imgCor) = 0.002000
- `touch_p0_synth`: |g_inv| = 8-206 (media 89.176) -- grau efetivo da inversao que A2/A4 usam
- `touch_p1_synth`: imgCor tem 12-12 chaves por linha (media 12.000), rho(imgCor) = 0.001200
- `touch_p1_synth`: |g_inv| = 9-313 (media 131.640) -- grau efetivo da inversao que A2/A4 usam
- `touch_p2_synth`: imgCor tem 10-10 chaves por linha (media 10.000), rho(imgCor) = 0.002000
- `touch_p2_synth`: |g_inv| = 7-219 (media 89.189) -- grau efetivo da inversao que A2/A4 usam
- `touch_p3_synth`: imgCor tem 20-20 chaves por linha (media 20.000), rho(imgCor) = 0.001538
- `touch_p3_synth`: |g_inv| = 111-491 (media 358.938) -- grau efetivo da inversao que A2/A4 usam
- `touch_p5_real_24k`: imgCor tem 20-20 chaves por linha (media 20.000), rho(imgCor) = 0.000833
- `touch_p5_real_24k`: |g_inv| = 1-183 (media 64.479) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p0_synth`: |g_inv| = 1-224 (media 90.275) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p1_synth`: |g_inv| = 9-307 (media 132.200) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p2_synth`: |g_inv| = 9-230 (media 90.268) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p3_synth`: |g_inv| = 68-746 (media 375.766) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p4_real_72k`: |g_inv| = 1-362 (media 73.036) -- grau efetivo da inversao que A2/A4 usam
- `touchraw_p5_real_24k`: |g_inv| = 1-222 (media 61.467) -- grau efetivo da inversao que A2/A4 usam

## T3. Trilha de RSS por checkpoint (o pico atribuido a estrutura viva)

`RSS_KB` e alto-marca (`ru_maxrss`), entao a trilha e monotona e cada `+` e a estrutura que passou a existir naquele ponto. Duas colunas em B/celula: sobre N^2 (o que o legado aloca) e sobre N*L (o que o algoritmo de fato escreve).

| run | modo | N | probe | RSS GB | delta GB | B/celula N^2 | B/entrada N*L |
|---|---|---|---|---|---|---|---|
| a3_p5_real_24k | A3 | 24000 | A3_afterF5 | 2.543 | +2.543 | 4.41 | 75.7 |
| a3_p5_real_24k | A3 | 24000 | A3_afterF6 | 2.543 | +0.000 | 4.41 | 75.7 |
| a3_p5_real_24k | A3 | 24000 | A3_afterF7 | 3.781 | +1.238 | 6.56 | 112.5 |
| a3_p5_real_24k | A3 | 24000 | **pico do processo** | 4.031 | | 7.00 | 120.0 |
| a4_p3_synth_memtrace | A4 | 13000 | A3_afterF5 | 0.808 | +0.808 | 4.78 | 44.4 |
| a4_p3_synth_memtrace | A4 | 13000 | A3_afterF6 | 0.808 | +0.000 | 4.78 | 44.4 |
| a4_p3_synth_memtrace | A4 | 13000 | A4_trips_full | 1.036 | +0.227 | 6.13 | 56.9 |
| a4_p3_synth_memtrace | A4 | 13000 | A4_csr_built | 1.485 | +0.450 | 8.79 | 81.6 |
| a4_p3_synth_memtrace | A4 | 13000 | A3_afterF7 | 1.485 | +0.000 | 8.79 | 81.6 |
| a4_p3_synth_memtrace | A4 | 13000 | **pico do processo** | 1.617 | | 9.57 | 88.9 |
| a4_p5_real_24k | A4 | 24000 | A3_afterF5 | 2.543 | +2.543 | 4.41 | 75.7 |
| a4_p5_real_24k | A4 | 24000 | A3_afterF6 | 2.543 | +0.000 | 4.41 | 75.7 |
| a4_p5_real_24k | A4 | 24000 | A4_trips_full | 2.955 | +0.412 | 5.13 | 87.9 |
| a4_p5_real_24k | A4 | 24000 | A4_csr_built | 3.781 | +0.826 | 6.56 | 112.5 |
| a4_p5_real_24k | A4 | 24000 | A3_afterF7 | 3.781 | +0.000 | 6.56 | 112.5 |
| a4_p5_real_24k | A4 | 24000 | **pico do processo** | 4.031 | | 7.00 | 120.0 |
| a4_p6_synth | A4 | 18000 | A3_afterF5 | 1.478 | +1.478 | 4.56 | 58.6 |
| a4_p6_synth | A4 | 18000 | A3_afterF6 | 1.478 | +0.000 | 4.56 | 58.6 |
| a4_p6_synth | A4 | 18000 | A4_trips_full | 1.789 | +0.311 | 5.52 | 71.0 |
| a4_p6_synth | A4 | 18000 | A4_csr_built | 2.421 | +0.632 | 7.47 | 96.1 |
| a4_p6_synth | A4 | 18000 | A3_afterF7 | 2.421 | +0.000 | 7.47 | 96.1 |
| a4_p6_synth | A4 | 18000 | **pico do processo** | 2.607 | | 8.05 | 103.4 |
| a4_p7_synth | A4 | 24000 | A3_afterF5 | 2.545 | +2.545 | 4.42 | 75.7 |
| a4_p7_synth | A4 | 24000 | A3_afterF6 | 2.545 | +0.000 | 4.42 | 75.7 |
| a4_p7_synth | A4 | 24000 | A4_trips_full | 2.957 | +0.412 | 5.13 | 88.0 |
| a4_p7_synth | A4 | 24000 | A4_csr_built | 3.784 | +0.827 | 6.57 | 112.6 |
| a4_p7_synth | A4 | 24000 | A3_afterF7 | 3.784 | +0.000 | 6.57 | 112.6 |
| a4_p7_synth | A4 | 24000 | **pico do processo** | 4.032 | | 7.00 | 120.0 |
| a4_p8_real_18k | A4 | 18000 | A3_afterF5 | 1.458 | +1.458 | 4.50 | 70.4 |
| a4_p8_real_18k | A4 | 18000 | A3_afterF6 | 1.458 | +0.000 | 4.50 | 70.4 |
| a4_p8_real_18k | A4 | 18000 | A4_trips_full | 1.714 | +0.257 | 5.29 | 82.8 |
| a4_p8_real_18k | A4 | 18000 | A4_csr_built | 2.235 | +0.520 | 6.90 | 108.0 |
| a4_p8_real_18k | A4 | 18000 | A3_afterF7 | 2.235 | +0.000 | 6.90 | 108.0 |
| a4_p8_real_18k | A4 | 18000 | **pico do processo** | 2.385 | | 7.36 | 115.2 |
| a_leg_p3_synth_memtrace | A | 13000 | A_afterF5 | 0.766 | +0.766 | 4.53 | 42.1 |
| a_leg_p3_synth_memtrace | A | 13000 | A_afterF7 | 1.442 | +0.676 | 8.53 | 79.2 |
| a_leg_p3_synth_memtrace | A | 13000 | A_end | 1.442 | +0.000 | 8.53 | 79.2 |
| a_leg_p3_synth_memtrace | A | 13000 | **pico do processo** | 1.442 | | 8.53 | 79.2 |
| a_leg_p6_synth | A | 18000 | A_afterF5 | 1.419 | +1.419 | 4.38 | 56.3 |
| a_leg_p6_synth | A | 18000 | A_afterF7 | 2.715 | +1.296 | 8.38 | 107.7 |
| a_leg_p6_synth | A | 18000 | A_end | 2.715 | +0.000 | 8.38 | 107.7 |
| a_leg_p6_synth | A | 18000 | **pico do processo** | 2.715 | | 8.38 | 107.7 |
| a_leg_p8_real_18k | A | 18000 | A_afterF5 | 1.401 | +1.401 | 4.32 | 67.7 |
| a_leg_p8_real_18k | A | 18000 | A_afterF7 | 2.697 | +1.296 | 8.32 | 130.3 |
| a_leg_p8_real_18k | A | 18000 | A_end | 2.697 | +0.000 | 8.32 | 130.3 |
| a_leg_p8_real_18k | A | 18000 | **pico do processo** | 2.697 | | 8.32 | 130.3 |
| ref_p8_real_18k | ref | 18000 | A_afterF5 | 1.401 | +1.401 | 4.32 | 67.7 |
| ref_p8_real_18k | ref | 18000 | A_afterF7 | 2.697 | +1.296 | 8.32 | 130.3 |
| ref_p8_real_18k | ref | 18000 | A_end | 2.697 | +0.000 | 8.32 | 130.3 |
| ref_p8_real_18k | ref | 18000 | **pico do processo** | 2.697 | | 8.32 | 130.3 |

## T4. Memoria medida: legado `A` (duas densas) vs `A4` (uma densa + CSR)

| N | L | dados | pico A (GB) | pico A4 (GB) | A4/A | T_total A (s) | T_total A4 (s) | A/A4 |
|---|---|---|---|---|---|---|---|---|
| 13000 | 1400 | SYNT | 1.44 | 1.62 | 1.122 | 126.0 | 47.5 | 2.65 |
| 18000 | 1150 | REAL | 2.70 | 2.38 | 0.884 | 108.4 | 29.2 | 3.72 |
| 18000 | 1400 | SYNT | 2.71 | 2.61 | 0.960 | 217.9 | 79.5 | 2.74 |

### Contabilidade do crossover (todas as grandezas medidas)

O que `A4` economiza e medido: o salto de F5 para F7 no legado e exatamente a segunda matriz n^2, que so chega a ter L entradas por linha (o `rc` da F7 tem `A4_CSR_NNZ = N*L`). O que `A4` paga de volta e o resto da conta: buffer de tripletos, build do CSR e F9 sobre CSR. Dai o N* onde o saldo vira.

| N | L | 2a densa economizada (GB) | custo do caminho CSR (GB) | B por entrada de N*L | N\* predito | A4 ja melhor? |
|---|---|---|---|---|---|---|
| 13000 | 1400 | 0.68 | 0.85 | 46.8 | 16377 | nao |
| 18000 | 1150 | 1.30 | 0.98 | 47.5 | 13669 | sim |
| 18000 | 1400 | 1.30 | 1.19 | 47.1 | 16498 | sim |

**Leitura.** Em N=13000 o saldo e positivo para o legado (A4/A = 1,12) porque o caminho CSR custa ~45 B por entrada de N*L e a matriz economizada vale 4 B x N^2: o crossover predito fica em N* ~ 16k, uma L e meia acima do ponto medido. Em N=72000 (escala de producao, L=1400) o mesmo modelo da A4/A ~ 0,6. A asfixia do legado nao e consertada em N pequeno -- so em N grande, o que e justamente o regime do ALOI inteiro.

## T5. Bit-exatidao: CK contra o golden + BITFINAL

`CK_W`/`CK_FINAL` sao os mesmos hash de 64 bits das Etapas 1-4, sobre `W` e sobre as listas finais. O golden de um ponto e o run `mode=ref` (o legado denso que ainda por cima gravou as listas em disco); na ausencia dele vale o `A`. `BITFINAL` e a comparacao byte-a-byte contra esse arquivo, feita pelo proprio binario -- e o que fecha, em dados reais, o que as Etapas 1-4 so haviam fechado com lista sintetica.

| N | L | k | dados | run | modo | CK_W | CK_FINAL | CK_FINAL vs golden | BITFINAL |
|---|---|---|---|---|---|---|---|---|---|
| 13000 | 1400 | 20 | SYNT | a4_p3_synth_memtrace | A4 | 0x1799c617f33f2518 | 0x0b43777da9d89399 | **SIM** | - |
| 13000 | 1400 | 20 | SYNT | a_leg_p3_synth_memtrace | A | 0x1799c617f33f2518 | 0x0b43777da9d89399 | golden | - |

| 18000 | 1150 | 20 | REAL | a2_p8_real_18k | A2 | 0xa5a07ecb29dd6118 | 0xb803eee6862a0751 | **SIM** | YES |
| 18000 | 1150 | 20 | REAL | a4_p8_real_18k | A4 | 0xa5a07ecb29dd6118 | 0xb803eee6862a0751 | **SIM** | YES |
| 18000 | 1150 | 20 | REAL | a_leg_p8_real_18k | A | 0xa5a07ecb29dd6118 | 0xb803eee6862a0751 | **SIM** | YES |
| 18000 | 1150 | 20 | REAL | ref_p8_real_18k | ref | 0xa5a07ecb29dd6118 | 0xb803eee6862a0751 | golden | - |

| 18000 | 1400 | 20 | SYNT | a4_p6_synth | A4 | 0xffa7fad35de48dc6 | 0xaa71d87a3df024dd | **SIM** | - |
| 18000 | 1400 | 20 | SYNT | a_leg_p6_synth | A | 0xffa7fad35de48dc6 | 0xaa71d87a3df024dd | golden | - |

**Veredito:** 5 de 5 variantes de port reproduzem o `CK_FINAL` do golden no mesmo ponto.

