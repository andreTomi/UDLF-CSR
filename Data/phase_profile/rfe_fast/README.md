# Fase 0 — Harness de avaliação + baseline do modo rápido RFE

**Objetivo do enunciado:** um modo rápido do RFE que seja **equivalente por
qualidade** ao legado. Antes de qualquer otimização, este dossiê estabelece o
*baseline* — o que medir, contra o que comparar, e onde o legado é inequívoco.

Branch `methods-legacy` @ `7fd7418`. **RFECore.hpp não foi modificado** (regra
da tarefa). Tudo roda em WSL (Debian 13, g++ 14.2, 12 threads, ~7.7 GB RAM).

## Artefatos

| Arquivo | O que é |
|---|---|
| `raw/table.csv` | consolidado: MAP@L, P@10, P@20, Recall@10, wall, RSS, ck_rk |
| `raw/summary.csv` | linhas wall/RSS/ck (fonte de `table.csv`) |
| `raw/parity.csv` | legado ≡ exato por config (`OK` / `NOT-EVALUATED`) |
| `raw/determinism.csv` | exato determinístico entre execuções (mesmo nº threads) |
| `raw/regime_ck.csv` | **Fase 0-B**: ck_rk do oráculo em 3 regimes de build |
| `raw/bridge_validation.txt` | ponte `Evaluation.hpp` real vs harness (Δ máx 2,7e-5) |
| `raw/gate_selftest.txt` | self-test do gate `compare` (fast=exact → PASS, Δ=0) |
| `Source/bench/rfe_quality.py` | métricas + gate `compare` (C1/C2/C3) |
| `Source/bench/bench_rfe_quality.cpp` | driver 1-processo-por-braço |
| `Source/tests/regime_probe.cpp` | oráculo serial p/ a medição de regime (0-B) |

## 1. Braços

- **(a) input** — o ranking de entrada, intacto.
- **(b) legacy** — `RFERef` (`rfe_ref_verbatim.hpp`): o RFE original copiado
  verbatim, o oráculo bit-exato. É o "legado real" validado na Fase 0-B.
- **(c) exact** — `RFECore` (`Source/src/Methods/hpp/RFECore.hpp`), o port CSR.

## 2. Paridade e determinismo (corel5k, linnaeus)

- **Paridade 16/16** (`parity.csv`): `ck_rk` legado ≡ exato em todos os 8
  configs por dataset (k∈{5,10,20} × L∈{400,1000} + rodada CC k=20). Batem com
  os pins do `step3b_real_gate.sh` (ex.: corel5k L400 k10 = `0xb2d84793a18b7e43`).
- **Determinismo 12/12** (`determinism.csv`): 2 reps, mesmo nº de threads, ck
  idêntico.

## 3. Validação ponte (`Evaluation.hpp` real vs harness)

`BRIDGEVERDICT,PASS` — 18 métricas, maior diff **2,7e-5** (só o arredondamento
de 6 dígitos do Logger). O harness `rfe_quality.py` espelha `Evaluation.hpp`
linha a linha; pode ser usado como juiz de qualidade.

## 4. Fase 0-B — "legado real" é inequívoco? (risco novo do enunciado)

O enunciado perguntou: *quais flags o Makefile original usa? Se o UDLF original
compila com fast-math, "bit-idêntico ao legado" fica ambíguo.* Medido
(`regime_probe.cpp`, oráculo verbatim compilado 3×):

| regime | flags | ck_rk (corel5k L400 k10) | MAP@400 |
|---|---|---|---|
| **R1 master** | `-std=gnu++14 -O3` (UDLF-master; **sem fast-math, sem omp**) | `0xb2d84793a18b7e43` | 10.961922 |
| **R2 strict** | `-O2 -ffp-contract=off -fopenmp` (gate deste repo) | `0xb2d84793a18b7e43` | 10.961922 |
| **R3 produto** | `CFLAGS` com `-march=native -mavx2 -mfma -ffast-math` | `0x926c15959f1cba0b` | 10.961920 |

**`REGIMEVERDICT,R1EQ2,R1NE3`.** Ou seja:
- **R1 ≡ R2, bit a bit** — e iguais ao pin do `step3b`. O gate estrito
  representa fielmente o build original; **o risco do "legado real" está
  eliminado** sem buildar o UDLF-master inteiro (o oráculo verbatim compilado
  com as flags do master reproduce os bits do binário original).
- **R3 (produto, fast-math) diverge** — confirma que o modo **EXACT** deve
  recusar fast-math (a recusa do carrier `RFE.hpp` está correta).

## 5. Tempo / memória — o exato já é 17–28× mais rápido que o legado

| config | legado wall/RSS | exato wall/RSS | speedup |
|---|---|---|---|
| corel5k L400 k10 | 9.7 s / 317 MB | 0.56 s / 153 MB | 17× |
| corel5k L1000 k20+CC | 121.0 s / 929 MB | 4.4 s / 582 MB | 28× |
| linnaeus L1000 k20+CC | 139.1 s / 1353 MB | 6.3 s / 952 MB | 22× |

**Consequência para o plano:** o ganho do modo rápido **não é velocidade** (o
exato já ganha do legado). O ganho é **memória** — o CC no exato materializa
matrizes densas N×N (RFECore `emb_by_cc_dense_`/`f8g_matrix_dense_`, ambos
`n*n`) que não cabem em ALOI. A Fase 1 (perfil) deve confirmar isso antes de
codar a Fase 2.

## 6. Escala ALOI (N=72000) — medido, não inferido

Descoberta que corrige o handoff: o exato **sem CC é CSR esparso** (`M =
CSRMatrix<float>`; o `sim_` do F7a tem nnz≈N×L), então **cabe** em ALOI:

| braço | L | wall | RSS (VmHWM) |
|---|---|---|---|
| exact | 400 | 3.5–10.3 s | 1.4–1.5 GB |
| exact | 1000 | 12–18.7 s | 3.4–3.5 GB |
| input | 400/1000 | 0 s | 0.11 / 0.28 GB |

Ganho real de qualidade em ALOI (input → exact), ex. L1000 k10:
MAP@L 43.77 → **52.40**, P@10 80.62 → **86.09**.

## 7. Não validado / fora da memória deste host (AVISO explícito)

- **ALOI legado** — `RFERef` materializa denso NxN = **20,74 GB** > 7,7 GB
  (WSL). **Recusado.** Exige host ≥32 GB.
- **ALOI exact + CC** — o `F8g A_Dense` (variante padrão) também é NxN = 20,74
  GB. **Recusado** até a Fase 3 emitir CSR direto de `cc_for_k1`.
- **Framework completo** (`make udlf-strict`) — **não builda neste host** (MKL
  ausente). A validação ponte usou `eval_bridge.cpp` (módulo de avaliação real,
  sem MKL).
- **R3 (produto fast-math)** — medido na Fase 0-B; o modo EXACT o recusa por
  design.

## 8. Reproduzir

```bash
bash Source/tests/run_regime_probe.sh        # Fase 0-B (3 regimes)
bash Source/tests/run_rfe_quality.sh bridge  # ponte Evaluation.hpp vs harness
bash Source/tests/run_rfe_quality.sh corel5k # ou linnaeus / aloi (resume-safe)
```

## 9. Critérios de aceitação (gate `compare`, implementado e auto-testado)

| critério | regra |
|---|---|
| C1 | \|ΔMAP\| e \|ΔP@n\| relativos ≤ 1% vs legado |
| C2 | desvio ≤ 10% do ganho RFE sobre o input |
| C3 | bootstrap pareado IC95 de (AP_rápido − AP_legado) contém 0 ou é todo ≥ 0 |
| (info) | Recall@n reportado; sobreposição top-n diagnóstico; determinismo por nº de threads |

Self-test (`gate_selftest.txt`): `fast=exact` (≡ legado por paridade) →
`QUALVERDICT,PASS,criteria=19`, Δ=0, overlap=1.0. Mecanismo validado para a
Fase 2.

