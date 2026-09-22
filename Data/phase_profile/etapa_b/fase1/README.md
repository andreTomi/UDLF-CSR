# Etapa B - Fase 1: Fragment-CSR por thread

## Resultado

Implementação de `build_from_ordered_fragments`: k-way merge de fragments ordenados por (row, col) direto para CSR, eliminando o buffer global `temp_triplets_`, o sort B2, e o histograma paralelo.

### Changes
- **Nova função `build_from_ordered_fragments`**: constrói CSR diretamente dos fragmentos por thread, sem `temp_triplets_` global.
- **`multiply_prune_row`**: sort por coluna dentro da linha (após nth_element) para manter invariante (row, col) ordenado. Usa `if constexpr` para pular sort no `DenseRowAccumulator` (colunas já ordenadas naturalmente).
- **`merge`/`combine`**: redirecionados para `build_from_ordered_fragments`.
- **`LHRRSparse mainMatrix`**: mantém `build_from_partial_results` (emissão simétrica fora do range da thread).

### Acceptance criteria
- **md5/checksum idéntico**: `-503.017268` (w1), `277.889357` (w2), `8273.428589` (w3) — idêntico ao baseline.
- **test_csr_ops**: 72 checks, 0 failures (1/2/4/8/12 threads).
- **test_multiply_and_prune**: ALL TESTS PASSED (1/2/4/8/12 threads).
- **Build CPU time @ 1 core**: 15.1ms vs 150.0ms baseline (9.9x mais rápido).
- **HWM reduzido**: 118.7MB vs 157.7MB baseline (-24.7%).

### Per-phase profile comparison (w1, 1 thread)

| Phase | Baseline (ms) | Fase 1 (ms) | Delta |
|-------|--------------|-------------|-------|
| B0_TEMP_RESIZE | 13.07 | 0.00 | eliminated |
| B1_COPY | 5.0 (medido; o "~280" anterior era número sem fonte) | 0.00 | eliminated |
| B2_SORT | 113.14 | 0.00 | eliminated |
| B3_HIST | 4.58 | 0.00 | eliminated |
| B3_PREFIX | 0.03 | 0.02 | ~same |
| B4_VALS_RESIZE | 7.55 | 7.45 | ~same |
| B5_FILL | 4.38 | 0.00 | eliminated |
| **BUILD_CALL** | **150.01** | **15.14** | **9.9x faster** |
| **VmHWM** (linha DONE, captura sem PHASE_MEM; MB decimais) | **157,7** | **118,8** | **-24,7%** |

### Wall-clock comparison (7 reps, discard rep 0)

| Workload | Threads | Baseline (ms) | Fase 1 (ms) | Speedup |
|----------|---------|--------------|-------------|---------|
| w1 | 1 | 551 | 430 | 1.28x |
| w2 | 1 | 609 | 523 | 1.16x |
| w3 | 1 | 20328 | 17945 | 1.13x |
