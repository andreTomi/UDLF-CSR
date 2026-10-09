# rfe_cc -- port de PERFORM_CCS (execRRCCs) para RFECore/CSRMatrix

Evidências do port CC, no mesmo formato das fases anteriores.

## Etapa 0 (baseline) + Etapa 1 (referência e goldens) -- 08/10/2026

- `golden_v1.log` -- saída canônica de `Source/tests/test_rfe_cc_golden.cpp`
  (11 casos x REPRO duplo; H11 directed: bound 34.666.. processa 35 arestas,
  a variante truncada selecionaria 34; H14 directed: unionCCs == união real;
  H16: embycc sem negativos/não-finitos). Final: `RFE CC GOLDEN: OK`.
- `cc_parity_etapa2.log` -- gate `Source/tests/test_rfe_cc_parity.cpp`
  (Etapa 2/F8a): core reproduz emb/sorted_emb/ee/sorted_ee pós-f7b
  bit-a-bit em 6 casos. Final: `RFE CC PARITY (Etapa 2/F8a): OK`.
- `cc_parity_etapa3.log` -- mesma porta crescida para a Etapa 3 (F8b): 11
  casos × 11 checks = 121 ok / 0 FAIL, variantes A e B de `f8a` ambas
  idênticas ao legado, `th_cc` bit-a-bit, prefixo == laço do legado.
  Invariância de threads: md5 `fbb3162775519b74f299baefacdf0487` (T=1/2/4/12).
- `etapa3_bench.md` -- seleção de variante: B (paralela) vence 2,6× em
  ALOI@T12 e 4,1× em corel5k@T12 com digest idêntico; B virou default.
  Instrumento: `Source/tests/bench_rfe_f8a.cpp` (reutilizável na Etapa 10).
  Também registra H19 (ALOI tem 552 rows com self fora do índice 0).
- `cc_parity_etapa4.log` -- gate CC crescido para a Etapa 4 (F8d): 14 casos ×
  15 checks = **210 ok / 0 FAIL** (icc+rcc idênticos para as variantes
  literal e DSU; casos incluem os 3 fixtures `cc_selfoff_*`). md5 único
  `86d68eb747d7098e65c03e9592622342` em T=1/2/4/12; ASan+UBSan limpos.
- `selfoff0_h19.log` -- gate H19 (opção a): **23 ok / 0 FAIL** — recusas duras
  (self ausente/OOR/duplicado), paridade F1 e final bit-idêntica em fixtures
  com self deslocado, `RKSELF_OFF0` verdadeiro. md5 único
  `6b5594cadcc4a7a96edaeb6131e6bf62` (T=1/12).
- `etapa4_bench.md` -- seleção F8d: DSU vence 116× em ALOI (3,7 vs 429,7ms)
  com digests idênticos → DSU é o default. Lacuna ALOI-legado declarada
  (5,7 GiB disponíveis vs 20,7 GB necessários) e receita para máquina ≥32 GiB.
- Pendência vinculada: `rk` final "com CCs" sobre fixtures deslocados fecha
  na Etapa 8/9 (pins já gravados no golden).
- Baseline gates (HEAD, antes do port): parity pins F1
  `ck_rk=0x3465e556d414ca79` / `ck_matrix=0x8afb15dd0502c7cb`; transcript md5
  `e3ef356f52d6565f5afde63af69a34c2` em t=1..12; corner 18/0; ASan+UBSan
  limpos. Números e hipóteses H10/H11/H13/H14/H16: `Source/tests/FINDINGS.md`
  (seção "Port CC (PERFORM_CCS)").
- Regenerar o golden: `bash Source/tests/run_all.sh` (etapa final) ou direto
  (WSL, na raiz do repo):
  `g++ -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I Source/src -I Source/src/Core/hpp -I Source/tests Source/tests/test_rfe_cc_golden.cpp -o /tmp/g && /tmp/g`
