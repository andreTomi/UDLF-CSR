# S2.1 — Builder v2: construtor único, ordem arbitrária, sem buffer global

Implementa o contrato único de construção (o objetivo declarado: "método novo não
precisa saber invariantes de emissão"). `build_from_partial_results` passa a ser o
**v2**; o construtor antigo virou `build_from_partial_results_legacy`, mantido
**somente** como referência do A/B. Nenhum método (`LHRRSparse`, `CprrSparse`) foi
alterado — o caso de exceção (`t2->row = neighbor`) deixou de ser exceção.

## Desenho (5 fases, todas instrumentadas)

`V1` histograma por (thread,linha) + detecção de `is_set` · `V2` prefixo de
`row_ptr_` **transformando o histograma in place em cursores absolutos** por thread
· `V3` scatter de cada stream nos seus slots (ordem = thread, depois emissão) ·
`V4` sort por linha + coalescência (paralelo por linhas, `schedule(dynamic,256)`) ·
`V5` compactação entre linhas, **serial por desenho** (com `new_ptr[r] ≤ old_ptr[r]`,
o destino da linha `r` pode invadir a *leitura* de linhas anteriores: `mmmove`
paralelo é race; crescente serial é seguro por construção).

Sem cópia do stream: nada de `temp_triplets_`, nada de `std::sort(par_unseq)` —
logo **sem dependência de TBB e sem sensibilidade ao bind** no builder.

## Portões medidos (ALOI LHRR, N=72000 L=400 k=10 limit=400, build da matriz de posição)

| métrica | legacy | v2 |
|---|---|---|
| `tmp_bytes` (`MEMBUILD`) | 921 600 000 | **0** |
| `ΔHWM` do build | 1 985 536 kB (1 939 MB) | **451 840 kB (441 MB) → −77%** |
| `bytes_per_triplet` | 35,298 | **8,033** |
| `count_bytes` | 1 440 000 | 1 440 004 (+4 B: `new_ptr`) |
| `out_bytes` | 321 723 648 | **321 723 648 (idêntico)** |
| nnz(posição) | 40 215 456 | **40 215 456** |

O `nnz` era o **pré-check falsificável**: 40 215 456 = 2·N·L − 17 384 544 recíprocos
previsto por medição independente no `rkLists` (30,2% de duplicatas). Bateu nas duas
vias.

## Gates de correção

| gate | resultado |
|---|---|
| `test_csr_ops` (agora 96 checks) + `test_multiply_and_prune` | PASSAM em T=1/2/4/8/12, **com asserts e com `-DNDEBUG`**; md5 da saída estável entre T |
| C19 `is_set` (substitui vs soma, 3 registros) | ✅ (a lacuna do fragment-CSR documentada) |
| C20 v2 vs legacy (mesmo conteúdo, distribuição de threads diferente, duplicatas) | ✅ suporte `(row,col)` idêntico, valores dentro de 1e-6 relativo |
| C21 emissão **fora da faixa da thread** | ✅ mesma matriz que a emissão in-range (o fragment-CSR rejeitaria) |
| C22a pareamento `(col,value)` adversarial, **nos dois modos de sort** | ✅ 0 desemparelhados |
| C22b colunas iguais/duplicatas sob os dois modos | ✅ |
| `PHASE_VALIDATE=1` (`S`,`Srev`,`Stot`,`result`) | ✅ ok nos 4, em v2 **e** legacy |
| Dump canônico no real: `MM` (40 215 456 entradas) | **BIT-IDÊNTICO** v2 vs legacy |
| Dump canônico sintético w1: 12 combinações (v2/legacy × idx/zip × T=1/4/12) | **md5 idêntico** em `ab.bin` e `result.bin` |

## O achado que o dump produziu (e que o checksum escondia)

No real, `H` e `result` têm **colunas idênticas e nnz idêntico**, mas valores
diferentes nos últimos bits:

| matriz | entradas | diferem | magnitude |
|---|---|---|---|
| `MM` | 40 215 456 | **0** | bit-idêntico |
| `H` | 1 982 199 | 242 126 (12,2%) | ≤ 1e-6 absoluto (~1 ULP) |
| `result` | 5 248 535 | 1 421 850 (27,1%) | ~1 ULP (relativo ~1e-7) |

Causa: `H` emite k²=100 registros por linha para ~27 células distintas → células com
**≥3 registros duplicados**, cuja ordem de soma é **indefinida nos dois builders**
(o `std::sort` do legacy é instável; `par_unseq` nem é determinístico). O v2 soma em
**ordem de emissão** (determinística e reprodutível); o legacy somava na ordem que o
sort instável deixou. Consequência prática: o `checksum` histórico
`119 396 289 001.122040` corresponde à ordem do **legacy**; o v2 dá
`119 396 288 415.396088` (−5,9e-9 relativo, 0 diferença estrutural). Registrar na
tese: o checksum é sensível a ordem de soma; a referência publicada é a do legacy.

## Zip iterator (modo `PHASE_ROWSORT=zip`)

Implementado e **usado**: exigiu `swap` por ADL no proxy (`iter_swap` recebe prvalue)
e comparador que aceita proxy **e** o `std::pair` interno do introsort. Validado pelo
C22a (pareamento) nos dois modos; saída **idêntica** ao modo `idx` nos 12 casos. A
medição comparativa dos dois modos fica no A/B (S2.2).

## Dívida conhecida registrada

1. **V5 é serial por desenho** (O(nnz); 321,7 MB movidos na ALOI, ~1-2% do build).
   Reavaliar se a fração de dedupe ou o nnz crescerem ordens de magnitude.
2. **Cursores `T × rows × 4 B`** (3,5 MB na ALOI; 256 MB em N=1M com T=64). Acima
   disso, histograma de dois níveis por blocos de linhas (desenho mapeado, não feito).
3. **Capacidade > size** após a compactação (slack de 139 MB na posição): evita
   `shrink_to_fit`, que reintroduziria o pico transitório.

## Reprodução

```
g++ -std=gnu++2b -O2 -fopenmp -DNDEBUG -DPHASE_PROFILE -I Source/src \
    Source/bench/bench_phase.cpp -o /home/andre/udlf_bin/bp_s2 -ltbb
# A/B de builders, dump canônico e comparador
bash Source/bench/s2_dump_matrix.sh /home/andre/dmp_matrix w1
PHASE_BUILDER={v2|legacy} PHASE_ROWSORT={idx|zip} PHASE_DUMP_DIR=<dir> PHASE_MEM=1 \
  bp_s2 real <rk.bin> 72000 400 10 400 lhrr 1
python3 Source/bench/dump_cmp.py <dirA>/MM.bin <dirB>/MM.bin
```
Knobs: `PHASE_BUILDER`, `PHASE_ROWSORT`, `PHASE_DUMP_DIR` (o driver imprime
`ABKNOBS,...` para cada rodada). O `dump` é `DUMP,<tag>,nnz=<n>`; o arquivo é
`rows,cols,nnz (u64) | row_ptr (u64) | por linha: col u32, valor u32`.

**Pendente (S2.2):** A/B same-session formal (ABBA + canário, política de descarte já
permanente) de tempo/ΔHWM por fase entre legacy e v2, com a decomposição do ΔHWM e do
**resíduo** do v2 (o desenho tem `tmp`/`out`/`counts` explícitos; o resíduo do legacy
era ~556 MB não atribuídos). Primeira indicação (T=4, 1 rep, mesma sessão):
**3 939 ms vs 8 279 ms**.
