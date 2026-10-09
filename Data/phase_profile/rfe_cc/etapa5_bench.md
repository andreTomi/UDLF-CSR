# Etapa 5 (F8e) -- medição de variantes: ccemb + sorted_ccemb

Protocolo: `Source/tests/bench_rfe_f8a.cpp` (seção F8E), build estrito,
1 warmup + 5 repetições, mediana e mínimo, `OMP_WAIT_POLICY=PASSIVE`, T=1
(fase sensível à ordem de membros; paralelismo é ENTRE CCs e não altera saída).

| dataset | CCs (≥2) | chaves | accum A (denso) | accum B (hash) | digest (igual) |
|---|---|---|---|---|---|
| ALOI N=72000 k=20 | 4.722 | 301.286 | **18,9ms** | 23,0ms | 0xa57fb2141e50aad8 |
| corel5k N=5000 k=20 | 254 | 41.830 | **2,9ms** | 3,3ms | 0x02352c597c9e8614 |

| dataset | sort A (completo) | sort B (rápido top-m) | fallbacks B |
|---|---|---|---|
| ALOI | 19,0ms (fallback n/a) | 18,7ms | **16 / 4.722 (0,3%)** |
| corel5k | 2,9ms | 2,4ms | **1 / 254 (0,4%)** |

**Decisões:**
- **accum A (denso com carimpo) mantido como default** — vence em ambos;
  hash só perde (sonda aberta + release por slot).
- **sort A (completo) mantido como default** — diferença de ~2% em ALOI e
  17% em corel, mas B muda o contrato do rabo (prefixo-m apenas; inalcançável
  pelo pipeline, mas instrumento mais fraco) → empate técnico, preferimos o
  mais simples com paridade TOTAL. B segue acessível
  (`set_f8_ccemb_fastsort(true)`); taxa de fallback medida (0,3–0,4%, quase
  tudo em CCs com empate no topo).

**Probe C (divergência do SpGEMM do engine) -- MEDIDA, não assumida** (linhas
`[C-probe]` do gate; a chamada de engine em si não foi replicada -- ver
FINDINGS pela nota de adiamento):
- Filtro `|v| > 1e-9` do `multiply_and_prune` descartaria **até 842/2744
  chaves (31%)** em ccemb (a maioria são os zeros exatos estruturais; casos
  com 0/500 também ocorrem).
- Sensibilidade à ordem: apenas INVERTER a ordem dos membros mudou **54 de 90
  chaves** num CC (e 5/51 em outros) -- a soma float não é comutativa aqui,
  então a ordem de acumulação do engine (paralela, sem garantia) divergiria.
- Conclusão: o engine, como variante de produção, é descartado por
  divergência medida; A/B internas são as candidatas legítimas.
