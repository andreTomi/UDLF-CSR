# `Source/bench` — arcabouço de medição de port

Não existe documento de régua neste repositório: a régua é o que
`Source/tests/run_all.sh`, `Source/Makefile` e `Source/src/Methods/` já fazem para
o RDPAC. Este arquivo registra **o protocolo que se repetiu em dois arcos** (RDPAC,
BFSTree) e o checklist que um método tem de passar para estar *incluído* no
framework — para o próximo arco não rediscover as armadilhas da §"Armadilhas".

Estado atual de cada método: **RDPAC** = incluído (fábrica + gates completos).
**BFSTree** = **incluído** na fábrica atrás do **alvo estrito** (`make
udlf-strict` → `bin/udlf_strict`; o binário do produto recusa rodá-lo) —
bit-exato em 3 pontos reais + paridade/corner/guard na malha. **A5** = pesquisa
documentada, não bloqueia. Ver `Data/phase_profile/bfstree_close/README.md` §7.4
e `Data/phase_profile/bfstree_a5/README.md`.

## O protocolo, em cinco passos

1. **Touch-first antes de medir qualquer coisa.** Um modo `TOUCH`/`TOUCHRAW` que
   só *lê* a estrutura e sai, sem alocar `W`. Em N=24k real, um touch de 20 s
   provou que a densidade sintética (`ρ(W) = 0,999`, o que fazia a previsão de
   −42% de pico) não era a densidade real (`≈ 0,02`) — e refutou a extrapolação
   antes de qualquer lote de horas. Custo do passo: um build e 20 s.
2. **Port bit-exato com golden embutido.** A referência verbatim vai para dentro
   do mesmo binário (modo `ref`), que roda primeiro, valida a hipótese que o port
   usa e **grava o golden** (`W` + `P@k`). O portador (`A2`/`A3`/`A4`) compara
   `CK_FINAL`, `BITFINAL` e P@k contra ele. Sem golden auto-gravado, "bit-exato"
   passa a significar "os dois erram igual".
3. **Driver com knobs, um caso por `.log`.** Modelo: `run_bfstree_step1.sh` —
   estágios (`touch_synth`, `real_rho`, `cross`, `real_bitex`) escolhidos por
   argumento, cada `run` gravando `<nome>.log`. O driver **declara o contrato de
   entrada dele mesmo** e recusa (`exit <rc>`) antes de rodar sobre dado inválido;
   o brutos ficam no diretório do dossiê.
4. **Agregador que imprime tabela, não número solto.** `bfstree_agg.py` /
   `bfstree_step1_agg.py` leem os `.log` e escrevem `aggregate.md`. Se a tabela do
   dossiê não é regenerável a partir dos brutos commitados, ela não existe.
5. **Dossiê com veredito e com o que *não* é claim.**
   `Data/phase_profile/<método>_close/README.md`, indexado em
   `Data/phase_profile/README.md`. O veredito diz a escala, o regime de threads e
   o build com que cada número foi obtido; o que não foi medido fica como débito
   numerado, não como omissão.

## Checklist de inclusão ("o método está no framework?")

É a régua que o arco do RDPAC deixou; os itens 1-6 vivem no produto, 7-9 na
medição.

| # | gate | como se prova (padrão do repositório) |
|---|---|---|
| 1 | método registrado na fábrica | `Source/src/Methods/cpp/Method.cpp`: `template class MethodRegister<M>;` + `MethodRegister<M> signInMethod("M")` |
| 2 | TU check | `Source/tests/check_<m>_tu.cpp`: as duas linhas acima + `MethodFactory::createMethod("<M>")`, sob `-fsyntax-only -Wall -Wextra` |
| 3 | paridade por fase vs o verbatim | `test_<m>_parity.cpp` contra `test_<m>_ref.cpp` (verbatim original embutido), dump comparado fase a fase |
| 4 | casos-limite multi-seed | `test_<m>_corner.cpp` — e os casos **vêm do censo de contrato dos dados reais**, não da imaginação (no BFSTree: `self` na posição 0 em só 71.448/72k linhas; recorte `id < N`; linha curta) |
| 5 | determinismo de threads | md5 da saída idêntico em **1/2/4/8/12 threads** (strict: drift aborta); md5 históricos não se tocam |
| 6 | ASan + UBSan | `OMP_NUM_THREADS=4`, como no `run_all.sh` |
| 7 | alvo de build próprio | `make test-<m>` em `Source/`, chamado também pelo `run_all.sh` |
| 8 | numérica do build **decidida** | ✅ o build do produto é `-O3 -march=native -mfma -ffast-math` e **permanece assim** (Passo 0 mediu que trocar muda o LHRR: `nnz` −8/11%, 81% das linhas — regime global em aberto); para método com contrato de identidade existe **alvo próprio**: `STRICT_FLAGS`/`make udlf-strict` (ver armadilha 4) e o método **recusa** rodar em fast-math |
| 9 | bench sob `OMP_WAIT_POLICY=PASSIVE` | spin-wait ACTIVE do libgomp esmaga os hyperthreads irmãos em host SMT (6c/12t) — causa-raiz diagnosticada no RDPAC |

Para um port que ainda mora só no bench, o equivalente mínimo é o
`Source/tests/gate_bfstree.sh` (itens 3/4/5 adaptados): hipótese + contrato de
entrada + `CK_FINAL` pinado. É o que o BFSTree tem hoje, e `make test-bfstree` é o
que o torna verificável por outra pessoa.

| 10 | **pin ABSOLUTO de regime** (X8) | pin de digest **hard** (`ck_gram` = bits crus de float, `ck_sim_dense`, `ck_rk` final) gravado do regime estrito e **asserido** — impresso nao e asseverado. A escada de regimes roda **sobre o pin**, nunca sobre um delta core-vs-referência: sob a mesma flag os dois lados sao perturbados juntos e o delta e invariante (medido no RFE: gate verde nos 4 regimes, `ck_rk` real `0x0db02fef...` sob NFM e `0x8079b48d...` sob PROD). Exercitado em **dado real grande o bastante** — o fixture de N=200 tolera 1 ULP, ALOI N=72000 com 63,52% de zeros exatos nao tolera. Item de **primeiro arco**, nunca auditoria tardia |

## Armadilhas que custaram runs

1. **CRLF em `.sh` salvo por ferramenta Windows.** Em WSL é erro de sintaxe duro
   (apesar de `.gitattributes` declarar `* text=auto eol=lf`, o arquivo *escrito*
   no Windows chega CRLF ao working tree). Correr com `tr -d '\r' < f > /tmp/f &&
   cp /tmp/f f`; conferir com `bash -n` antes de confiar. Custou dois lotes.
2. **Editar um driver com lote em curso.** O `bash` lê o script incrementalmente:
   reimprimir o arquivo desloca o byte-offset de leitura e o lote morre num `;;`
   aleatório no meio de um estágio. Lote em curso = arquivo congelado.
3. **`min(N, L)` como guarda de `L`.** Em dados reais o filtro `id < N` derruba o
   número de sobreviventes por linha: `L=1400` não é atingível em N=18k (mínimo
   observado 1171). O carregador certo **recusa** (`rc=5`) em vez de preencher com
   lixo — guarda de contrato é asserção, não `min`.
4. **`-ffast-math` do produto ≠ `-ffp-contract=off` do gate.** Medido no encerramento
   do BFSTree (`bfstree_close/README.md` §7.2): em `-O2` o `CK_FINAL` não muda com
   ou sem a flag; sob as flags do produto (`-O3 -ffast-math`) o port CSR deixa de
   ser bit-exato contra o legado. Gate pinado em `-O2` é contrato **de `-O2`**.
5. **md5 do suite ≠ md5 do binário.** Suite que imprime a contagem de threads no
   cabeçalho exige `tail -n +2` antes do `md5sum`; suite que imprime tempos por
   fase nunca dá `cmp` de saída inteira — compara-se só a linha de checksum.

6. **Gate de igualdade que compara coleção vazia passa.** Um gate que itera um
   conjunto vazio reporta `0 divergências` e dá **verde** — passou sem medir
   nada. Ocorreu duas vezes no arco do RFE (F7), e na pior hora: o gate que
   existia para pegar pipeline em ordem errada estava ele próprio em ordem
   errada, reportando sucesso sobre zero células. Regra: todo gate de paridade
   precisa de **duas** asserções — `mismatches == 0` **e** `compared > 0` — e
   deve imprimir "comparei N" na mesma linha do "0 divergências". Consertar só
   a ordem das chamadas deixa a armadilha armada para a fase seguinte; o conserto
   é **estrutural** (a iteração vazia vira falha). Princípio geral, registrado
   como X6 em `../tests/FINDINGS.md`.
7. **Gate relativo é cego a mudança de regime por construção.** Um gate que
   compara o core contra a referência compilada **com as mesmas flags** não pode
   detectar que o regime mudou: `-ffast-math` perturba os dois lados
   identicamente, então `mismatches == 0` persiste enquanto todo valor absoluto
   derivou. Não é gate mal escrito — é gate cujo instrumento não existe. Medido
   no RFE (Step 3 / Stage 3c): `test_rfe_parity` imprimiu `RFE PARITY ...: OK`
   nos **quatro** regimes, com `ck_gram` impresso e nunca asseverado (linha 789),
   enquanto em ALOI real o `ck_rk` final ia de `0x0db02fefdf6bd62c` (NFM/BENCH)
   a `0x8079b48d44cbd874` (PROD/PRODC). Conserto estrutural: **pin absoluto do
   instrumento duro, asseverado, exercitado sobre a escada de regimes em dado
   real grande o bastante**. Registrado como X8 em `../tests/FINDINGS.md`.
   Nota de atribuição: aqui o culpado foi **reassociação**, não contração de FMA
   (`PRODC == PROD`), ao contrário do que o precedente LHRR sugeria — a escada
   tem de ser medida por método, nunca herdada.

## Inventário

| arquivo | papel |
|---|---|
| `bench_e2e.cpp`, `bench_phase.cpp`, `bench_accumulator.cpp`, `micro_omp.cpp` | kernel CSR adaptativo e micro-benchmarks das fases 0-2 |
| `bench_rdpac.cpp`, `bench_bfstree.cpp` | bancos com tempos por fase e golden verbatim embutido |
| `run_bfstree_campaign.sh`, `run_bfstree_step1.sh`, `run_phase1_validation.sh`, `s2*_run.sh`, `s1_bis_run.sh`, `ab_run.sh` | drivers com estágios; um caso por `.log` |
| `bfstree_agg.py`, `bfstree_step1_agg.py`, `aggregate_phase.py`, `aggregate_etapa_a.py`, `ab_agg.py`, `a5bis_agg.py`, `s2*_agg.py` | agregadores → `aggregate.md` |
| `rk_width.py`, `rk_self_census.awk`, `rk_cmp.py`, `pk_eval.py` | censo estrutural das listas reais (`ρ(W)`, largura da lista, posição do `self`, P@k) |
| `../tests/gate_bfstree.sh` | gates do port BFSTree na malha (`run_all.sh`, `make test-bfstree`) |
| `_*.sh`, `_*.py` **nesta pasta e na raiz** (19 arquivos versionados na raiz) | diagnósticos das fases 0-2, citados por `Data/phase_profile/etapa_b/s24/README.md`, `etapa_b/s2_close/README.md`, `bfstree_step1/aggregate.md`, `canary.sh` e `run_bfstree_step1.sh`. Decidido em 27/09/2026: **ficam onde estão** — são versionados e referenciados por esses 6 arquivos; mover exige reescrever as referências, e elas são registro histórico de corrida |

