# Etapa B - Fase 3: nth_element sobre pares contíguos

## Resultado: Fase 3 é contra-producente — recomendo reverter

Implementação de `nth_element` sobre pares contíguos `(col, val)` no lugar do
`nth_element` indireto sobre `used[]` com comparador `acc.val_of(slot)`. O código
é correto (md5/checksums idênticos), mas a **medição mostra que é mais lento**.

### Por que é mais lento

A Fase 3 adiciona dois custos por linha:
1. **Materialização de pares**: `O(touched)` — percorre `used[]` + `col_of()` + `val_of()`
2. **Remove o `if constexpr` da Fase 1**: o sort por coluna passa a ser sempre executado,
   mesmo no `DenseRowAccumulator` onde `col_of(slot) = slot` (já ordenado)

O ganho esperado — comparador contíguo no `nth_element` — não se materializa porque:
- `DenseRowAccumulator.val_of(slot) = val[slot]` (contiguous vector) — os lookups
  já são rápidos
- `HashRowAccumulator.slot_val[slot]` (contiguous vector) — também rápidos, pois
  `bound` é pequeno (≤400) e os slots são acessados em ordem de inserção (não
  aleatória dentro de uma linha)

### Medição (w1/w2/w3, 1 thread)

| Workload | Fase (variant) | F1_ACCUM (ms) | F2_PRUNE (ms) | MP_CALL (ms) | REP (ms) |
|----------|---------------|---------------|---------------|--------------|----------|
| w1 | baseline | 191.6 | 168.6 | 528.8 | 528.8 |
| w1 | Fase 1 | 255.6 | 209.6 | 490.0 | 490.0 |
| w1 | **Fase 1+3** | 224.4 | **247.6** | **496.9** | **496.9** |
| w2 | baseline | 340.2 | 207.4 | 731.9 | 731.9 |
| w2 | Fase 1 | 247.8 | 227.3 | 500.1 | 500.1 |
| w2 | **Fase 1+3** | 312.7 | **275.3** | **615.5** | **615.5** |
| w3 | baseline | 10,114.7 | 8,330.7 | 20,717.6 | 20,719.7 |
| w3 | Fase 1 | 10,169.3 | 8,306.0 | 18,796.8 | 18,799.5 |
| w3 | **Fase 1+3** | 10,215.1 | **9,052.0** | **19,470.0** | **19,471.9** |

### Multi-thread scaling (Fase 1+3)

| Workload | t=1 | t=2 | t=4 | t=8 | t=12 |
|----------|-----|-----|-----|-----|------|
| w1 | 630 ms | 233 ms | 125 ms | 108 ms | 88 ms |
| w2 | 497 ms | 305 ms | 170 ms | 92 ms | 87 ms |
| w3 | 19,234 ms | — | — | — | — |

### Checksums (idênticos, mas mais lento)

| Workload | Checksum | Status |
|----------|----------|--------|
| w1 | -503.017268 | ✅ idêntico |
| w2 | 277.889357 | ✅ idêntico |
| w3 | 8273.428589 | ✅ idêntico |
| md5 (CSR ops) | b04547bd… | ✅ idêntico 1-12 threads |
| md5 (multiply) | 112fb487… | ✅ idêntico 1-12 threads |

### Recomendação

**Reverter a Fase 3** (restaurar o código da Fase 1 sem a materialização de pares)
e manter apenas a Fase 1 (fragment-CSR). A Fase 1 já dá os ganhos medidos:
- BUILD_CALL 9.9× mais rápido
- HWM -24.7%
- w1/w2/w3 wall-clock 1.28×/1.16×/1.13× mais rápido
