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

## Conclusão que mudou o plano — RESOLVIDA (parar no exato)

O exato (`RFECore`) já é 17–28× mais rápido que o legado com ~½ da memória, e é
bit-exato (16/16). Logo o modo rápido **não se justifica por velocidade**.
Decisão: **parar no exato** — nenhum `RFEFastCore`, nenhum knob O1–O4. O CC
(F8a→F8h) já está integrado e gateado; CC-em-escala (ALOI, denso n×n = 20,7 GB)
fica **fora de escopo** neste host (< 32 GB RAM). Restou só **fechar o carrier**
(correções pontuais abaixo) e documentar — feito.

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

## Estado final — projeto encerrado ("parar no exato")

O modo rápido pedido == o `RFECore` (exato): bit-exato, 17–28× mais rápido, CC
integrado e gateado. Não há Fases 1–5 a executar. Feito nesta rodada (carrier):

- **C2** `RFE.hpp`: comentário/log "PERFORM_CCS NOT honoured -- WIP" (falso desde
  a Etapa 9) corrigido. Opt-in `RFE_ALLOW_CC=1` **mantido** (decisão: não mexer
  no CC). Validação: `make test-rfe` OK + RFE.hpp TU compile OK.
- **C3** `run_all.sh`: `cd` absoluto apontava path **inexistente** (abortava na
  l.29 neste workspace) → virou relativo `$(dirname "$0")/../.."`; cache ALOI
  hardcoded → configurável via `ALOI_BIN`. Provado: passa da l.29 de cwd neutro.

Se no futuro CC-em-escala (ALOI) for requisitado, o único trabalho é a **Fase 3
original**: emitir F8g/F8h em CSR direto de `cc_for_k1` (sem a matriz densa n×n
de 20,7 GB), em host com ≥32 GB RAM. Nada mais.

## Armadilhas deste host (confirmadas nesta rodada)

- **WSL reinicia entre comandos e WIPA o `/tmp` (tmpfs):** jobs em background
  (`setsid`/`nohup &`) morrem; intermedios devem ficar em mount persistente e as
  etapas serem **síncronas**. A derivação ALOI (3 GB via `/mnt/c`) levou 80–145 s.
- Scripts `.sh`/`.cpp` escritos no Windows chegam com **CRLF**:
  `sed -i 's/\r$//'` antes de `bash -n`.
- `bc` não existe (usar `awk`); `make udlf-strict` falta MKL.
- PowerShell quebra `|`/parênteses dentro de `wsl bash -c "..."` — prefira
  scripts em arquivo.
