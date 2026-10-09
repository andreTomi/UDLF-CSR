# Etapa 3 (F8b) -- medição de variantes: arestas de CC (f8a_create_edges_ccs)

Protocolo: `Source/tests/bench_rfe_f8a.cpp`, build estrito
`-O2 -ffp-contract=off -fopenmp`, 1 warmup + 5 repetições, mediana e mínimo,
`OMP_WAIT_POLICY=PASSIVE`, estado (emb/ee/rk pós-f7b) montado uma vez --
`f8a` apenas lê. Digest = FNV-1a sobre (conf_bits, q, nn) da aresta ordenada.

| dataset | E | threads | A mediana | B mediana | digest (igual) |
|---|---|---|---|---|---|
| ALOI N=72000 k=20 | 1.368.000 | 1 | 396,6ms (min 395,5) | 403,3ms (min 402,6) | 0xaba4e09e2916cb27 |
| ALOI N=72000 k=20 | 1.368.000 | 4 | 405,8ms | 214,1ms | 0xaba4e09e2916cb27 |
| ALOI N=72000 k=20 | 1.368.000 | 12 | 401,3ms (min 395,1) | 155,3ms (min 155,2) | 0xaba4e09e2916cb27 |
| corel5k N=5000 k=20 | 95.000 | 1 | 59,6ms | 60,7ms | 0x1ae8b941cc09ea8e |
| corel5k N=5000 k=20 | 95.000 | 12 | 60,6ms | 14,8ms | 0x1ae8b941cc09ea8e |

**Decisão: variante B (default)** -- 2,6x em ALOI@T12 e 4,1x em
corel5k@T12; custo de ~1,7% em T=1 aceito. Digest idêntico entre modos E
entre contagens de threads (invariância medida, não assumida). Piso serial
restante em ambos: o `std::sort` final (~130ms dos ~400ms de A); ordem
(conf,q,nn) é TOTAL, então sort paralelo seria bit-idêntico também --
**adiado** (sem TBB, implementação manual desproporcional para ~4% do
pipeline), reavaliar só se o perfil da Etapa 10 mostrar `f8a` quente.

Observação (H19): o bench executa a sequência do `run()` SEM
`validate_input()` no setup — o arquivo ALOI real tem 552/72000 rows com self
fora do índice 0 (todas CONTÉM self). **H19 RESOLVIDA pela opção (a)** em
08/10/2026 (validate relaxado para "self presente"; gate `test_rfe_selfoff0`);
ver FINDINGS (seção H19) e `etapa4_bench.md`.
