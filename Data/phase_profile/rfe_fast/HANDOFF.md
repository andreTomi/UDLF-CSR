# HANDOFF — Fase 0 concluída → Fases 1–5

Estado: **Fase 0 ~100%** (corel5k, linnaeus, ALOI-parcial medidos). Branch
`methods-legacy` @ `7fd7418` + trabalho não commitado (ver "Commit").

## O que a Fase 0 provou (resumo)

1. **Paridade legado≡exato 16/16** e **determinismo 12/12** em corel5k/linnaeus
   (ck batem com os pins do `step3b`).
2. **Ponte** `Evaluation.hpp` real vs harness: Δ máx 2,7e-5 — o harness
   `rfe_quality.py` é juiz confiável.
3. **Fase 0-B (risco novo do enunciado) — RESOLVIDO:** o oráculo verbatim
   compilado com as flags do master (`-O3`, sem fast-math/omp) dá o **mesmo
   ck_rk** que o gate estrito (`R1≡R2`), e difere do produto fast-math (`R3`).
   "Legado real" é inequívoco: é o RFERef sob R1/R2; EXACT recusa R3.
4. **Escala ALOI medida:** o exato **sem CC é CSR esparso e CABE** (RSS 1.4 GB
   L400 / 3.5 GB L1000). Só o CC (denso NxN = 20,74 GB) e o legado denso não
   cabem neste host — recusados e documentados.
5. **Gate `compare` auto-testado** (`fast=exact` → PASS, 19 critérios, Δ=0).

## Conclusão que muda o plano (leia antes da Fase 2)

O exato já é **17–28× mais rápido** que o legado com ~½ da memória. Logo o modo
rápido **não se justifica por velocidade**. O único ganho real é **memória no
CC** (o `F8g A_Dense`/`emb_by_cc_dense_` densos NxN não cabem em ALOI). A
**Fase 1 (perfil) deve vir primeiro** e pode concluir que o único alvo que
vale é o **CC em CSR** (Fase 3), não O1–O4 de velocidade.

## Arquivos novos (untracked / a comitar)

```
Source/bench/rfe_quality.py        # métricas + gate compare (C1/C2/C3)
Source/bench/bench_rfe_quality.cpp # 1 processo por braço; wall/VmHWM/ck_rk
Source/bench/eval_bridge_check.py  # comparador módulo-legado vs harness
Source/bench/agg_table.py          # consolida run_*.log -> table.csv
Source/tests/eval_bridge.cpp       # driver Evaluation.hpp real (sem MKL)
Source/tests/run_rfe_quality.sh    # sweep Fase 0 (5 parches aplicados)
Source/tests/regime_probe.cpp      # oráculo serial p/ Fase 0-B
Source/tests/run_regime_probe.sh   # medição dos 3 regimes
Data/phase_profile/rfe_fast/       # raw/*.csv, logs, README.md
RFE_PORT_SUMMARY.md                # resumo do port (doc existente)
```

`Data/phase_profile/rfe_fast/work/` (derivações ALOI ~560 MB) está em
`.gitignore` — regenerável, não commitado.

## RFECore.hpp: NÃO MODIFICADO (regra da tarefa). RFEFastCore.hpp: não existe.

## Próximos passos

1. **Commit de checkpoint** (feito; ver histórico).
2. **Fase 1 — perfil:** `bench_rfe_census` com `RFE_FULL=1 RFE_F6PROF=1
   RFE_F1PROF=1` em corel5k/linnaeus/ALOI → ranking das fases por custo.
   Publicar o ranking ANTES de codar. Hipótese: F6 (Gram) e F8g/F8h (CC) dominam.
3. **Fase 2 — RFEFastCore.hpp** (cópia com knobs), S0 sanidade (todos OFF ≡
   exato). Só se a Fase 1 mostrar alvo de velocidade — senão, ir direto à Fase 3.
4. **Fase 3 — F8g/F8h CSR direto de `cc_for_k1`** (sem NxN; B_CSR quebrada NÃO
   portada). É o que habilita CC em ALOI. Indexar storage por chunk `c`.
5. **Fase 4 — escala ALOI** rápido vs exato (RSS/tempo), CC ligado (só no rápido).
6. **Fase 5 — carrier** `RFE.hpp`: `PARAM_RFE_MODE=EXACT|FAST`; fast-math/canário
   só em EXACT; atualizar comentários desatualizados (PERFORM_CCS WIP).

## Armadilhas deste host (confirmadas nesta rodada)

- **WSL reinicia entre comandos e WIPA o `/tmp` (tmpfs):** jobs em background
  (`setsid`/`nohup &`) morrem; intermedios devem ficar em mount persistente e as
  etapas serem **síncronas**. A derivação ALOI (3 GB via `/mnt/c`) levou 80–145 s.
- Scripts `.sh`/`.cpp` escritos no Windows chegam com **CRLF**:
  `sed -i 's/\r$//'` antes de `bash -n`.
- `bc` não existe (usar `awk`); `make udlf-strict` falta MKL.
- PowerShell quebra `|`/parênteses dentro de `wsl bash -c "..."` — prefira
  scripts em arquivo.
