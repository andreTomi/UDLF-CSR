
## POINT 0: N=5000 L=100 k=20  (`bfstree_campaign_0.log`)

| mode | B | limit | T_TOTAL | speedup vs A | T_F5 | T_F6 | T_F7 | T_F9 | RSS MB | CK_FINAL | bit-exact |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ref | 0 | 5000 | 5.73 s | 1.00x | 4.00 | 0.42 | 0.33 | 0.01 | 201 | `a275be4a` | CK-chain |
| A | 0 | 5000 | 5.70 s | 1.00x | 3.96 | 0.42 | 0.37 | 0.01 | 201 | `a275be4a` | YES |
| A2 | 256 | 5000 | 3.90 s | 1.46x | 2.78 | 0.41 | 0.13 | 0.02 | 167 | `a275be4a` | YES |
| A3 | 256 | 5000 | 3.38 s | 1.69x | 2.64 | 0.04 | 0.15 | 0.02 | 167 | `a275be4a` | YES |
| A4 | 256 | 5000 | 3.39 s | 1.68x | 2.66 | 0.04 | 0.15 | 0.02 | 166 | `a275be4a` | YES |
| A2 | 1024 | 5000 | 3.59 s | 1.59x | 2.48 | 0.43 | 0.13 | 0.02 | 178 | `a275be4a` | YES |
| A3 | 1024 | 5000 | 3.32 s | 1.72x | 2.53 | 0.05 | 0.19 | 0.02 | 178 | `a275be4a` | YES |
| A4 | 1024 | 5000 | 4.01 s | 1.42x | 3.20 | 0.04 | 0.15 | 0.02 | 178 | `a275be4a` | YES |
| A2 | 4096 | 5000 | 3.83 s | 1.49x | 2.69 | 0.43 | 0.16 | 0.02 | 231 | `a275be4a` | YES |
| A3 | 4096 | 5000 | 3.34 s | 1.71x | 2.58 | 0.04 | 0.17 | 0.02 | 231 | `a275be4a` | YES |
| A4 | 4096 | 5000 | 3.34 s | 1.71x | 2.58 | 0.04 | 0.17 | 0.02 | 232 | `a275be4a` | YES |
| A3a | 256 | 5000 | 3.82 s | 1.49x | 3.01 | 0.12 | 0.14 | 0.02 | 203 | `45e058b6` | CK-chain |
| F6a | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| E | 0 | 5000 | 7.66 s | 0.74x | 1.35 | 0.66 | 5.21 | 0.02 | 1415 | `9cf76a92` | CK-chain |
| E | 0 | 800 | 6.05 s | 0.94x | 0.62 | 0.13 | 4.87 | 0.02 | 480 | `0ec8fe9e` | CK-chain |
| E | 0 | 200 | 4.72 s | 1.21x | 0.47 | 0.04 | 3.78 | 0.02 | 197 | `9c8ce4e2` | CK-chain |
| E | 0 | 100 | 4.09 s | 1.39x | 0.43 | 0.02 | 3.20 | 0.02 | 154 | `f5126c82` | CK-chain |
| E | 0 | 20 | 2.64 s | 2.16x | 0.42 | 0.00 | 1.78 | 0.02 | 116 | `d22b9f56` | CK-chain |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |

**Bit-exactness chain.** CK_FINAL grouped over the exact modes (A, A2, A3, A4):

- `0xd9456d10a275be4a` <- A(B=0), A2(B=256), A3(B=256), A4(B=256), A2(B=1024), A3(B=1024), A4(B=1024), A2(B=4096), A3(B=4096), A4(B=4096)

CHECK: **unique CK_FINAL -> chain bit-exact**

BITFINAL=YES against the golden reference for: A, A2, A3, A4, A2, A3, A4, A2, A3, A4

**F6a gate** (algebraic colsum vs legacy colsum, cell by cell):

| N | BIT | GATE_OK | BEYOND | MAXREL | MAXABS |
|---|---|---|---|---|---|
| 5000 | 0 | 5000 | 0 | 5.21e-07 | 2.96e-06 |

**E (native SpGEMM) limit sweep** -- `limit` = max entries kept per W row:

| limit | T_TOTAL | POS_EQUAL | P@1 | P@5 | P@10 | P@20 | NNZ_W |
|---|---|---|---|---|---|---|---|
| 5000 | 7.66 s | 0.999880 | 1.000000 | 1.000000 | 1.000000 | 1.000000 | 24906720 |
| 800 | 6.05 s | 0.404558 | 1.000000 | 0.999840 | 0.999820 | 0.998820 | 7976269 |
| 200 | 4.72 s | 0.235598 | 1.000000 | 0.999600 | 0.999440 | 0.985560 | 2499891 |
| 100 | 4.09 s | 0.185004 | 1.000000 | 0.999280 | 0.996380 | 0.959940 | 1204536 |
| 20 | 2.64 s | 0.093610 | 1.000000 | 0.994240 | 0.882160 | 0.809230 | 218124 |

**Eattr W-level error attribution** (share of the N^2 cells, legacy W vs SpGEMM W):

| limit | bit-equal | 1e-9 filter | float-round within gates | float-round beyond gates | pruned | spurious | maxrel beyond |
|---|---|---|---|---|---|---|---|
| - | 53.94% | 0.36% | 45.68% | 0.00% | 0.02% | 0.00% | 0 |
| - | 0.51% | 0.36% | 4.18% | 0.14% | 94.82% | 0.00% | 0.982 |

## POINT 1: N=10000 L=100 k=12  (`bfstree_campaign_1.log`)

| mode | B | limit | T_TOTAL | speedup vs A | T_F5 | T_F6 | T_F7 | T_F9 | RSS MB | CK_FINAL | bit-exact |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ref | 0 | 10000 | 7.87 s | 1.00x | 1.74 | 2.10 | 1.02 | 0.02 | 776 | `f08dd0cf` | CK-chain |
| A | 0 | 10000 | 7.87 s | 1.00x | 1.72 | 2.10 | 1.03 | 0.02 | 776 | `f08dd0cf` | YES |
| A2 | 256 | 10000 | 4.73 s | 1.66x | 1.05 | 2.24 | 0.42 | 0.05 | 462 | `f08dd0cf` | YES |
| A3 | 256 | 10000 | 3.02 s | 2.60x | 1.12 | 0.31 | 0.51 | 0.06 | 462 | `f08dd0cf` | YES |
| A4 | 256 | 10000 | 3.15 s | 2.50x | 1.16 | 0.29 | 0.57 | 0.05 | 462 | `f08dd0cf` | YES |
| A2 | 1024 | 10000 | 5.14 s | 1.53x | 1.27 | 2.29 | 0.46 | 0.05 | 490 | `f08dd0cf` | YES |
| A3 | 1024 | 10000 | 2.47 s | 3.18x | 0.87 | 0.22 | 0.38 | 0.05 | 490 | `f08dd0cf` | YES |
| A4 | 1024 | 10000 | 2.36 s | 3.33x | 0.75 | 0.23 | 0.39 | 0.05 | 490 | `f08dd0cf` | YES |
| A2 | 4096 | 10000 | 4.43 s | 1.77x | 0.84 | 2.11 | 0.49 | 0.05 | 616 | `f08dd0cf` | YES |
| A3 | 4096 | 10000 | 3.11 s | 2.53x | 1.22 | 0.27 | 0.61 | 0.05 | 617 | `f08dd0cf` | YES |
| A4 | 4096 | 10000 | 2.93 s | 2.68x | 1.09 | 0.25 | 0.53 | 0.06 | 617 | `f08dd0cf` | YES |
| A3a | 256 | 10000 | 2.58 s | 3.05x | 1.11 | 0.08 | 0.37 | 0.05 | 475 | `a58e6faf` | CK-chain |
| F6a | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| E | 0 | 10000 | 17.73 s | 0.44x | 2.49 | 1.65 | 13.28 | 0.05 | 3403 | `dfc4d0db` | CK-chain |
| E | 0 | 800 | 10.92 s | 0.72x | 0.68 | 0.36 | 9.58 | 0.05 | 823 | `c7ade337` | CK-chain |
| E | 0 | 200 | 8.35 s | 0.94x | 0.45 | 0.10 | 7.50 | 0.05 | 395 | `45417d1b` | CK-chain |
| E | 0 | 100 | 7.20 s | 1.09x | 0.33 | 0.04 | 6.54 | 0.04 | 395 | `686ce2b3` | CK-chain |
| E | 0 | 12 | 3.95 s | 1.99x | 0.37 | 0.01 | 3.25 | 0.03 | 395 | `dc3ef17f` | CK-chain |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |

**Bit-exactness chain.** CK_FINAL grouped over the exact modes (A, A2, A3, A4):

- `0xc6211586f08dd0cf` <- A(B=0), A2(B=256), A3(B=256), A4(B=256), A2(B=1024), A3(B=1024), A4(B=1024), A2(B=4096), A3(B=4096), A4(B=4096)

CHECK: **unique CK_FINAL -> chain bit-exact**

BITFINAL=YES against the golden reference for: A, A2, A3, A4, A2, A3, A4, A2, A3, A4

**F6a gate** (algebraic colsum vs legacy colsum, cell by cell):

| N | BIT | GATE_OK | BEYOND | MAXREL | MAXABS |
|---|---|---|---|---|---|
| 10000 | 0 | 10000 | 0 | 5.55e-07 | 2.12e-06 |

**E (native SpGEMM) limit sweep** -- `limit` = max entries kept per W row:

| limit | T_TOTAL | POS_EQUAL | P@1 | P@5 | P@10 | P@20 | NNZ_W |
|---|---|---|---|---|---|---|---|
| 10000 | 17.73 s | 0.997869 | 1.000000 | 1.000000 | 1.000000 | 1.000000 | 66006321 |
| 800 | 10.92 s | 0.307423 | 1.000000 | 0.999920 | 0.999970 | 0.996290 | 15398561 |
| 200 | 8.35 s | 0.193431 | 1.000000 | 0.999800 | 0.999700 | 0.955080 | 4625452 |
| 100 | 7.20 s | 0.163025 | 1.000000 | 0.999540 | 0.998230 | 0.900335 | 2514184 |
| 12 | 3.95 s | 0.087434 | 1.000000 | 0.986180 | 0.896950 | 0.684605 | 282439 |

**Eattr W-level error attribution** (share of the N^2 cells, legacy W vs SpGEMM W):

| limit | bit-equal | 1e-9 filter | float-round within gates | float-round beyond gates | pruned | spurious | maxrel beyond |
|---|---|---|---|---|---|---|---|
| - | 90.76% | 6.38% | 2.86% | 0.00% | 0.00% | 0.00% | 0 |
| - | 28.64% | 6.38% | 1.48% | 0.01% | 63.49% | 0.00% | 0.996 |

## POINT 2: N=5000 L=100 k=10  (`bfstree_campaign_2.log`)

| mode | B | limit | T_TOTAL | speedup vs A | T_F5 | T_F6 | T_F7 | T_F9 | RSS MB | CK_FINAL | bit-exact |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ref | 0 | 5000 | 1.86 s | 1.07x | 0.38 | 0.45 | 0.32 | 0.01 | 199 | `015b6e02` | CK-chain |
| A | 0 | 5000 | 1.99 s | 1.00x | 0.44 | 0.44 | 0.40 | 0.01 | 198 | `015b6e02` | YES |
| A2 | 256 | 5000 | 1.04 s | 1.91x | 0.17 | 0.45 | 0.15 | 0.02 | 156 | `015b6e02` | YES |
| A3 | 256 | 5000 | 0.66 s | 3.00x | 0.18 | 0.04 | 0.16 | 0.02 | 156 | `015b6e02` | YES |
| A4 | 256 | 5000 | 0.66 s | 3.04x | 0.16 | 0.04 | 0.17 | 0.02 | 155 | `015b6e02` | YES |
| A2 | 1024 | 5000 | 1.08 s | 1.85x | 0.19 | 0.44 | 0.15 | 0.02 | 167 | `015b6e02` | YES |
| A3 | 1024 | 5000 | 0.63 s | 3.19x | 0.16 | 0.04 | 0.15 | 0.03 | 166 | `015b6e02` | YES |
| A4 | 1024 | 5000 | 0.62 s | 3.20x | 0.17 | 0.04 | 0.14 | 0.02 | 167 | `015b6e02` | YES |
| A2 | 4096 | 5000 | 1.08 s | 1.85x | 0.18 | 0.45 | 0.16 | 0.02 | 221 | `015b6e02` | YES |
| A3 | 4096 | 5000 | 0.67 s | 3.00x | 0.18 | 0.04 | 0.17 | 0.02 | 221 | `015b6e02` | YES |
| A4 | 4096 | 5000 | 0.70 s | 2.86x | 0.20 | 0.04 | 0.18 | 0.02 | 221 | `015b6e02` | YES |
| A3a | 256 | 5000 | 0.61 s | 3.27x | 0.17 | 0.03 | 0.14 | 0.02 | 153 | `015b6e02` | YES |
| F6a | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| E | 0 | 5000 | 7.18 s | 0.28x | 0.66 | 0.47 | 5.92 | 0.03 | 874 | `374ca05e` | CK-chain |
| E | 0 | 800 | 5.31 s | 0.38x | 0.27 | 0.13 | 4.80 | 0.02 | 377 | `271f90a6` | CK-chain |
| E | 0 | 200 | 3.90 s | 0.51x | 0.14 | 0.03 | 3.62 | 0.03 | 147 | `8402e6d2` | CK-chain |
| E | 0 | 100 | 3.41 s | 0.58x | 0.11 | 0.01 | 3.17 | 0.02 | 106 | `cf58b9f2` | CK-chain |
| E | 0 | 10 | 1.61 s | 1.24x | 0.11 | 0.00 | 1.40 | 0.02 | 103 | `ad9ae572` | CK-chain |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |
| Eattr | - | - | gate-only | - | - | - | - | - | - | - | see gate table |

**Bit-exactness chain.** CK_FINAL grouped over the exact modes (A, A2, A3, A4):

- `0xffa8768d015b6e02` <- A(B=0), A2(B=256), A3(B=256), A4(B=256), A2(B=1024), A3(B=1024), A4(B=1024), A2(B=4096), A3(B=4096), A4(B=4096)

CHECK: **unique CK_FINAL -> chain bit-exact**

BITFINAL=YES against the golden reference for: A, A2, A3, A4, A2, A3, A4, A2, A3, A4

**F6a gate** (algebraic colsum vs legacy colsum, cell by cell):

| N | BIT | GATE_OK | BEYOND | MAXREL | MAXABS |
|---|---|---|---|---|---|
| 5000 | 0 | 5000 | 0 | 3.07e-07 | 1.68e-06 |

**E (native SpGEMM) limit sweep** -- `limit` = max entries kept per W row:

| limit | T_TOTAL | POS_EQUAL | P@1 | P@5 | P@10 | P@20 | NNZ_W |
|---|---|---|---|---|---|---|---|
| 5000 | 7.18 s | 0.999684 | 1.000000 | 1.000000 | 1.000000 | 1.000000 | 16468558 |
| 800 | 5.31 s | 0.388346 | 1.000000 | 0.999920 | 0.999980 | 0.998070 | 6589368 |
| 200 | 3.90 s | 0.204764 | 1.000000 | 0.999720 | 0.999320 | 0.967530 | 2055349 |
| 100 | 3.41 s | 0.160628 | 1.000000 | 0.999560 | 0.996340 | 0.912700 | 1152000 |
| 10 | 1.61 s | 0.074488 | 1.000000 | 0.972040 | 0.876340 | 0.617510 | 108503 |

**Eattr W-level error attribution** (share of the N^2 cells, legacy W vs SpGEMM W):

| limit | bit-equal | 1e-9 filter | float-round within gates | float-round beyond gates | pruned | spurious | maxrel beyond |
|---|---|---|---|---|---|---|---|
| - | 95.52% | 1.88% | 2.60% | 0.00% | 0.00% | 0.00% | 0 |
| - | 34.25% | 1.88% | 2.56% | 0.04% | 61.27% | 0.00% | 0.995 |

## POINT 3: N=13000 L=1400 k=20  (`bfstree_campaign_3.log`)

| mode | B | limit | T_TOTAL | speedup vs A | T_F5 | T_F6 | T_F7 | T_F9 | RSS MB | CK_FINAL | bit-exact |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A2 | 256 | 13000 | 60.76 s | - | 8.26 | 3.80 | 40.60 | 4.35 | 1542 | `a9d89399` | CK-chain |
| A3 | 256 | 13000 | 57.30 s | - | 8.50 | 0.44 | 39.77 | 4.74 | 1542 | `a9d89399` | CK-chain |
| A4 | 256 | 13000 | 53.90 s | - | 11.87 | 0.44 | 32.89 | 4.28 | 1542 | `a9d89399` | CK-chain |
| A2 | 1024 | 13000 | 62.33 s | - | 8.08 | 3.87 | 41.70 | 4.82 | 1566 | `a9d89399` | CK-chain |
| A3 | 1024 | 13000 | 76.18 s | - | 12.32 | 0.52 | 54.08 | 4.87 | 1566 | `a9d89399` | CK-chain |
| A4 | 1024 | 13000 | 50.32 s | - | 8.17 | 0.51 | 33.16 | 4.24 | 1536 | `a9d89399` | CK-chain |
| F6a | - | - | gate-only | - | - | - | - | - | - | - | see gate table |

**Bit-exactness chain.** CK_FINAL grouped over the exact modes (A, A2, A3, A4):

- `0x0b43777da9d89399` <- A2(B=256), A3(B=256), A4(B=256), A2(B=1024), A3(B=1024), A4(B=1024)

CHECK: **unique CK_FINAL -> chain bit-exact**

**F6a gate** (algebraic colsum vs legacy colsum, cell by cell):

| N | BIT | GATE_OK | BEYOND | MAXREL | MAXABS |
|---|---|---|---|---|---|
| 13000 | 0 | 13000 | 0 | 5.91e-07 | 4.43e-06 |
