# PORT_STATE — ledger de estado (atualizar em TODA sessão)

Legenda de status: `—` não iniciado · `EM ANDAMENTO` · `FEITO` · `BLOQUEADO(motivo)`.
Classe: D0–D4 (ver DIVERGENCE_CLASSES.md). "Herdado" = vem do trabalho anterior, NÃO revalidado por esta metodologia.

## Fundação
| Tarefa | Status | Data | Evidência / observação |
|---|---|---|---|
| T0 baseline + commit | FEITO | 2026-10-13 | Escopo adaptado pela F0: checkpoint pré-existente (HEAD `eb933a1`, 81 commits); baseline transcrito de `Data/phase_profile/rfe_fast/raw/` → `docs/BASELINE.md`. Lacuna: % de posições divergentes R1×R3 não medida. |
| T1 shim do Udl | FEITO | 2026-10-13 | `UDL_sandbox/` (shim+driver, decisão do humano: tudo no sandbox). RFE corel5k L400 k10 **bate o pin** `0xb2d84793a18b7e43`; MAP@400=10.961922 = regime_ck; determinismo 2/2. Pins NOVOS CPRR/LHRR gerados (aguardam aceite humano). Relatório: `UDL_sandbox/T1_REPORT.md` |
| T2 biblioteca de comparação | FEITO | 2026-10-13 | `UDL_sandbox/compare/` (decisão T1: tudo no sandbox). 24/24 testes + 5 mutações obrigatórias detectadas, normais e com ASan+UBSan; vazio → exit 2; smoke real: RFE×si=D0, inerte=D4. N_ULP=4 **provisório** (pendente no DECISIONS). Achado: RFE original troca ids das linhas vs entrada (ck==pin; contrato p/ K-a). Saída: `UDL_sandbox/compare/test_output.txt` |
| T3 formato de snapshot | FEITO | 2026-10-13 | `UDL_sandbox/snapshot/` (formato SNP1, `FORMAT.md`). Round-trip D0 via T2: densa, triplas c/ zeros explícitos, ranking, CSR; integridade (byte corrompido rejeitado); golden obsoleto → `snapshot_verify` rc=3 (bin e src hash). 10/10 testes normais e ASan+UBSan. Evidência: `UDL_sandbox/snapshot/test_output.txt`. Captura no original = K-a |
| T4 notas do motor | FEITO | 2026-10-13 | `UDL_sandbox/engine_tests/` (E1–E10, 10/10 verdes + ASan/UBSan limpos; suites versionadas como evidência). E1–E4,E6–E8,E10 CONFIRMADOS; **E5 REFUTADO** (ranges são dica, :231); **E9 PARCIAL** (`nnz()` é size_t); **E2b: builder preserva zeros explícitos** (pivô das regras). **BUG REAL achado**: use-after-free em `VirtualArray.hpp:47` (PITFALLS #19, prova ASan) — NÃO corrigido (motor read-only). Regras de uso + "o que escrever" em ENGINE_NOTES.md; TSan não roda no host. Evidência: `UDL_sandbox/engine_tests/test_output.txt` + `asan_evidence/` |
| T5 fixtures | FEITO | 2026-10-13 | `UDL_sandbox/fixtures/` + `Datasets/catalog.json`. 13/13 fixtures válidas (6 sintéticas + 2 escala on-demand + 5 reais: corel5k, linnaeus, brodatz, cars, cifar10 com classes e proveniência). QA exploratório nos 9 datasets; anomalias de fonte documentadas (linnaeus 5, cars 39, aloi 552, vegfru 43 linhas). Corpus D-0004: Tier1 legado+esparso / Tier2 só esparso / pets fora. Evidência: `UDL_sandbox/fixtures/test_output.txt`, `dataset_qa_output.txt` |
| T6 validação do harness (CPRR/LHRR) | FEITO | 2026-10-14 | `ports/_validation/REPORT.md` **completo (7/7)**. t1 corrida REFUTADA (20×, 1/2/8 threads); t2 70k sem overflow (esparso 301 MB; original recusado 39,2 GB); t3–4 LHRR **D2+D3 medidos** (1874 vs 3,97e6; resetDB/tmpList ausentes); t5 determinismo REFUTADO nas fixtures testadas (pesos inteiros; risco residual E3 fracionário anotado); t6 **contradição resolvida**: NÃO há inércia, MAP é insensível (asis ≈ orig com 95–99% das posições diferentes — D3 em 6/6), linnaeus×LHRR original degrada 74.16→71.99; t7 **CONFIRMADA bug do original** (ASan `Cprr.cpp:401` imgRef n·l) + par_unseq/uint32 REFUTADOS. Harness detectou tudo |
| T7 catálogo dos métodos | FEITO | 2026-10-13 | `ports/_catalog/` (METHODS.md + 5 por método). Tabela comparativa, kernels com arquivo:linha, params/gates, ordenação/tipos, memória denso vs esparso a n=72000, estado entre iterações, quirks herdados, prévia M2. Kernels homônimos mapeados (não são reuso). Recomendação: CPRR como piloto. Aguarda revisão do humano antes do M1 |
| T8 reformulação CPRR | FEITO | 2026-10-13 | `ports/_reformulation/` (README schema + CPRR.md K1–K7). **Achado: `CprrSparse.hpp` NÃO é port fiel** — 8 divergências mapeadas (pesos k-j vs l-pos, reset por iteração, multiply_and_prune com prune/filtro/float, std::sort instável vs insertion estável, self inserido quando ausente, params renomeados). Especificação fiel por kernel + review points K-c. RFE/RDPAC/LHRR/BFSTree pendentes |
| M1 RFE (mapa do algoritmo) | FEITO | 2026-10-14 | `ports/RFE/ALGO_MAP.md` criado; 39 funções, 20 kernels, glossário 10 termos, M2 proposta baseada em `RFECore.hpp` existente; fusão FORA DE ESCOPO. |
| T8 reformulação LHRR | FEITO | 2026-10-13 | `ports/_reformulation/LHRR.md` K1–K9. Só K1 (fillPos) é fiel; 8 divergências: sem `resetDB(1)` (produto parte dos pesos l-pos), sem `confid`/produto cartesiano/`tmpList` (zeros = descoberta de candidatos), multiply_and_prune vs similaridades double, float vs double, quirks do insertion sort (j=2,`i>0`), self inserido, params renomeados. RFE/RDPAC/BFSTree pendentes |

## Métodos
| Método | M1 mapa | M2 estrutura | Kernels | Pilha | FINAL | Observação |
|---|---|---|---|---|---|---|
| CPRR (piloto) | FEITO (2026-10-14, `ports/CPRR/ALGO_MAP.md`) | FEITO (2026-10-14, `ports/CPRR/ALGO_MAP.md` §8) | K-a/K-b executados; auditoria da referência em `ports/CPRR/K_c_REFERENCE_AUDIT.md`: 52 verificações, sem mismatch K1–K6 | **K-c VALIDADO (2026-10-16)** em `ports/CPRR/K_c_VALIDATION.md` conforme D-0005/D-0006: listas idênticas nas 4 execuções sem bucket reverso excedido; 4 fixtures com `nRef>L` comparadas à referência contratual; 3/3 mutações detectadas; replay determinístico; ASan/UBSan; escala n=72000; qualidade corel5k/brodatz. ASan OOB original confirmado para hub; não extrapolar OOB global às demais fixtures. Thread-count/TSan N/A (serial). | **Port K-b validado; pronto para integração controlada** em `Source`, que ainda precisa de teste ponta a ponta. Não otimizar antes da validação da integração |
| LHRR | — | — | — | — | — | herdado: ver "Suspeitas" |
| RDPAC | — | — | — | — | — | herdado: port existente, não revalidado |
| BFSTree | — | — | — | — | — | herdado: W ainda densa n×n |
| RFE | — | — | — | — | — | herdado: Fase 0 fechada (paridade 16/16, determinismo 12/12, decisão "parar no exato" @ `eb933a1`); NÃO revalidada sob esta metodologia (mutações, sanitizers, fixtures adversariais pendentes) |

## Kernels (uma linha por kernel; preencher na M1/M2)
| Método | Kernel | Original | Esparso v1 | Classe | Determ. | Sanit. | Mutações | MAP | Relatório |
|---|---|---|---|---|---|---|---|---|---|
| (vazio) | | | | | | | | | |

## Estado herdado (reportado antes desta metodologia; NÃO verificado aqui)
- **RFE:** core exato (`RFECore.hpp`) **bit-idêntico ao oráculo `RFERef`** em corel5k e linnaeus:
  paridade 16/16 (`parity.csv`), determinismo 12/12 (`determinism.csv`), ~17–28× mais rápido e ~½ da
  memória (README Fase 0). Ponte do avaliador validada (Δ máx 2,7e-5). Regimes de build medidos
  (Fase 0-B): R1 ≡ R2, R3 fast-math diverge (ver `BASELINE.md`). ALOI **parcial medido**: exact sem CC
  é CSR e cabe (1,4 GB L400 / 3,5 GB L1000); legado denso e CC denso (n×n ≈ 20,7 GB) recusados neste
  host (< 32 GB RAM). Decisão registrada: **parar no exato** (commit `eb933a1`, transcrita como D-0002).
  Ainda pendente de revalidação sob ESTA metodologia: mutações plantadas, sanitizers (ASan/UBSan/TSan),
  fixtures adversariais, hipóteses do motor (T4), `parallel for` por chunk indexado por
  `omp_get_thread_num()` (PITFALLS #10), CCs (F8g/F8h) sem prova em dados reais neste ciclo.
- **BFSTree:** núcleo reportado bit-exato "por construção", mas W continua n×n densa; só RBO; recusa fast-math;
  `build_csr` com o problema do PITFALLS #10.
- **RDPAC:** esparso de verdade (O(N·L)); sem trava de fast-math; depende de `OMP_WAIT_POLICY=PASSIVE` para ter speedup;
  K_END > L pode divergir do original; fusão não portada.
- **Avaliador:** harness de MAP/P@n validado contra `Evaluation.hpp` (18 métricas, diff máx. 2,7e-5).

## Suspeitas a testar na T6 (hipóteses de leitura de código, NÃO confirmadas)
- **CPRR:** corrida de dados no `kernelFillPosMatrix` (paralelo sem locks, escrita espelho); `uint32_t` overflow em n>65535;
  `imgRef` pode estourar se um item aparece em mais de `l` listas; `sort par_unseq` muda empates.
- **LHRR:** matriz `uint32_t` trunca valores fracionários; falta re-zerar a matriz no início de cada iteração
  (original chama `initSparseMatrix` antes de `resetDB`); overflow de índice; ordem de `tmpList` não determinística.
- **Contradição a explicar:** foi reportado "MAP idêntico em 3 datasets e vários parâmetros" para esses métodos,
  o que não combina com as suspeitas acima. Esclarecer: idêntico a quê? Comparou listas, ou só MAP? Havia a linha da entrada?
- **VirtualArray:** ponteiro pendente em `create()`, `assign(0)` no-op, nome de arquivo fixo.

## Pendências globais
- **Trabalho anterior ESTÁ commitado** (corrigido na F0): branch `methods-legacy` @ `eb933a1`,
  81 commits, **+31 não pushados** sobre `origin/methods-legacy` @ `cb90f53` (2026-09-29).
- **Política de arquivos (decisão do humano, F0):** `AGENTS.md`, `docs/`, `ports/`, `udlf_original/`
  ficam **untracked** — são material de metodologia/estado para agentes, nunca serão commitados.
  O passo 1 da T0 (commit em `wip-before-restart` incluindo untracked) está **cancelado** por isso.
- **Push dos 31 commits: FEITO (2026-10-13).** `origin/methods-legacy` @ `8345bd3` (fast-forward
  `cb90f53..8345bd3`). Os 27 commits do `Audit Freeze` foram **reautorados** para
  `andreTomi <andrutomitan@gmail.com>` (identidade já presente no histórico), mensagens e datas
  preservadas, árvore idêntica ao backup. Backup de rollback: branch `backup/pre-push-20261013`.
  Untracked de metodologia (`AGENTS.md`, `docs/`, `ports/`, `udlf_original/`, `UDL_sandbox/`)
  e `Datasets/` (git-ignored, incl. `catalog.json`) **não** foram commitados/pushed.
  Scripts: `UDL_sandbox/push_prep.sh`, `UDL_sandbox/filter_env.sh`.
- Nota de ambiente: 4029 arquivos internos do `.git` estavam com atributo ReadOnly (processo
  "freeze"); atributo removido para permitir o reflog. Arquivos fora do `.git` seguem como estavam.
- **Fast-math do original:** RESOLVIDO na Fase 0-B — flags do Makefile = `-I./src -std=gnu++14 -O3`
  (sem fast-math); regime R3 (produto, fast-math) diverge de R1/R2. Ver `docs/BASELINE.md`;
  transcrição como D-0001 em `docs/DECISIONS.md` aguarda confirmação humana.

## Marcos em andamento
| Método | M1 | M2 | K-b (correção) | K-c | Status |
|---|---|---|---|---|---|
| CPRR | FEITO (2026-10-14, `ports/CPRR/ALGO_MAP.md`) | **FEITO (2026-10-14, `ports/CPRR/ALGO_MAP.md` §8)** | **VALIDADO** — replay 2×8 fixtures / 224 hashes; 3 mutações detectadas; ASan/UBSan 8/8; sonda n=72000 sem overflow; qualidade `QUALVERDICT,PASS` em Corel5k/Brodatz | **K-c VALIDADO sob D-0005/D-0006** — ver `ports/CPRR/K_c_VALIDATION.md`. Sem divergência de lista nos casos sem bucket excedido; nos quatro casos `nRef>L`, port coincide com semântica K1–K6 contratual sem limite `L`; snapshots do original preservados, não usados como oracle. Comparador rerun oficial em `Cs_2_lists`; os demais candidatos repetem byte a byte K-b arquivado. **Integração controlada executada**: `Source/src/Methods/hpp/CprrSparse.hpp` agora chama os kernels seriais de `CprrSparseCore.hpp`. O gate dedicado valida rankings 8/8 strict, fast-math e ASan/UBSan, repetibilidade e métricas Corel5k/Brodatz; fluxo `MethodFactory`/`Method::run()` conferido em `syn_n47`. Relatório: `ports/CPRR/INTEGRATION_REPORT.md`. Build integral `make udlf` bloqueado neste host por falta de `mkl.h`; nenhuma flag global foi alterada. Métricas integradas conferidas, mas a suíte global de qualidade com todos os critérios não foi rerodada. | Integração implementada; aguarda aceite explícito antes da otimização. Build do produto bloqueado pela dependência MKL ausente neste ambiente. |
| CPRR K-d baseline/O1/O3-K3/O4 rows | **EM ANDAMENTO** | 2026-10-16 | Frozen serial oracle `ports/CPRR/CprrSparseSerialOracle.hpp`; profiler `ports/CPRR/benchmark_serial.cpp`; details `ports/CPRR/PROFILE_PLAN.md`. O1 transfers wrapper-owned ranking into state (one full `n*L` copy removed). O2 stable sort measured and reverted. O3-K3 serial destination-row gather passes exact rankings; O4 enables OpenMP only for independent K2/O3-K3/K6 rows. Final gate `ports/CPRR/integration_final_o4.log`: strict 8/8, fast-math 8/8, ASan/UBSan 8/8, repeatability, quality and framework smoke. Ten random seeds, hub, ties and dense k=L passed. Corel5k exact differential passed at 1/2/6/12 threads. Candidate-only wall 1.184/0.869/0.679/0.647 s, speedups 1.00/1.36/1.74/1.83x. OpenMP ASan/UBSan hub+ties passed; TSan host abort was `unexpected memory mapping`. K1/K4 remain serial; no large-scale result. Candidate-only HWM ~36 MiB vs oracle-only ~44 MiB. Agreed `72k×400,k=100,t=3` attempt stopped after ~3 min at ~566 MiB and ~96% CPU before output; incomplete, not OOM. |

## Cronograma (marcos) — estimativa em sessões, registrada 2026-10-14

| Marco | Conteúdo | Sessões | Status |
|---|---|---|---|
| M-CPRR | M1 ✓ → M2 ✓ → K-a ✓ → K-b executado → K-c comparação/regressões; decisão humana pendente pelas classes D2/D3 → otimização só após aceite | 3–4 | **K-c parcial/bloqueado** (`ports/CPRR/REPORT_K_c.md`) |
| M-LHRR | M1+M2 → K-b (resetDB, confid/tmpList/cartProd, double, quirks, VLA) → K-c (spec pronta em `_reformulation/LHRR.md`) | 3–4 | pendente |
| M-BFSTree | M1+M2 → K-b (W esparso — o ganho de memória morre no W denso hoje) → K-c | 3 | pendente |
| M-RDPAC | M1+M2 → K-b (fidelidade: K_END>L, trava fast-math, fusão; W/RWL/DBL esparso) → K-c | 3 | pendente |
| M-RFE | **FEITO (2026-10-14, `ports/RFE/ALGO_MAP.md`)** | pendente | — | pendente | M1 FEITO; **K-a FEITO** (driver único `ports/RFE/K_a_orig.cpp`, CONTRACT.md, goldens: 6 sintéticas MAIN+CC completas e determinísticas; corel5k MAIN parcial C0..C9 — C5/C10 F6 excede tempo por chamada); M2 por revisão humana |
| FINAL | carrier E2E + avaliador + sonda de escala | 1 | pendente |
| K-d (otimizações) | opcionais até benchmark exigir | +1–2 | condicional |

**Total ≈ 13–17 sessões** (~1–1,5 semana no ritmo atual) + paradas D2+ do humano (0,5–1 cada).
Pressupostos: harness (T1–T6) concluído; cada sessão reporta contra esta tabela.
