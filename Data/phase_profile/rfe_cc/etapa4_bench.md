# Etapa 4 (F8d) -- medição de variantes: componentes conexas (processCCs)

Protocolo: `Source/tests/bench_rfe_f8a.cpp` (agora mede F8A e F8D), build
estrito, 1 warmup + 5 repetições, mediana e mínimo, `OMP_WAIT_POLICY=PASSIVE`.
Fase serial por design (a ordem do prefixo define os rótulos) — threads não
afetam; medida com T=1.

| dataset | E | prefixo (th auto) | A literal (map/set) | B DSU | ganho |
|---|---|---|---|---|---|
| ALOI N=72000 k=20 | 1.368.000 | 283.722 (th=0,207398862) | 429,7ms (min 390,0) | **3,7ms (min 3,4)** | **116×** |
| corel5k N=5000 k=20 | 95.000 | 16.225 (th=0,170783743) | 9,9ms (min 9,1) | **0,16ms** | **63×** |

- Digests de rótulos **idênticos** entre A e B em todos os runs
  (ALOI `0x563bfd8f691eaa7d`, corel `0xfdda28d7ac0f7030`) — paridade A≡B≡legado
  (o gate `test_rfe_cc_parity` compara as duas contra a referência nos 14 casos).
- Custos medidos: ~400ms do tempo de A é a **construção de 72000 `std::set`**
  antes da primeira união; as uniões em si custam ~30ms. O DSU faz tudo em
  arrays.
- **Decisão: B (DSU) é o default** (`f8_components_dsu_ = true`); A continua
  acessível via `set_f8_components_dsu(false)` para teste diferencial e é o
  que o gate força na chamada "A".

Nota (H19, resolvido — ver FINDINGS): o prefixo acima usa a regra automática
de `th_cc` sobre o ALOI real; o bench executa a sequência do `run()` sem
`validate_input` apenas no setup (o gate `test_rfe_selfoff0` cobre a validação
agora relaxada de ponta a ponta).
