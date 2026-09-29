# Numérica do motor sob as flags do produto (`numerics_prod`)

Auditoria que responde três perguntas sobre a numérica do build **de produção**
(`Source/Makefile`, extraído por `make -C Source -s show-flags`, nada
parafraseado): **(Q1)** os valores do motor mudam quando o código sai das flags
da malha (`-O2`) e entra nas do produto (`-O3 -march=native -mfma -ffast-math
-DNDEBUG …`)? **(Q2)** o pin bit-a-bit do port BFSTree (dossiê `bfstree_close`
§7.2) sobrevive a essas flags — e qual parte delas o quebra? **(Q3)** quanto
custa, em tempo, o regime que preserva os bits?

Driver: `Source/bench/prod_numerics.sh` (estágios `flags | suites | values |
golden | timing | verdict`); protocolo geral em `Source/bench/README.md`.
Execução completa registrada em `run2.log`
(`NUMERICS_STAGE_suites golden timing verdict_DONE`). Cada linha do veredito é
um `sed`/`awk` sobre o artefato bruto do mesmo diretório — nada digitado à mão.

## Método: os quatro regimes

| regime | string de flags | papel |
|---|---|---|
| **PROD** | literal do `Optimization flags:` do Makefile (inclui `-O3 -march=native -mtune=native -mavx2 -mfma -msse4.2 -ffast-math -funsafe-math-optimizations -ffinite-math-only -DNDEBUG -fopenmp …`) | o que o produto compila hoje |
| **PRODC** | PROD **+ `-ffp-contract=off`** | isola a **contração de FMA** do resto do `-ffast-math` (atribuição de causa) |
| **NFM** | PROD **−** `-ffast-math -funsafe-math-optimizations -ffinite-math-only` **+ `-ffp-contract=off`** | candidato da decisão (1) do dossiê §7.4 |
| **BENCH** | `-std=gnu++2b -O2 -fopenmp` (literal do `run_all.sh`) | âncora: sem ela os md5 registrados no repo não seriam comparáveis |

Guardas no pré-âmbulo (sem elas a auditoria passaria por vacuidade): PROD tem
de conter `-ffast-math`; NFM tem de diferir de PROD e não conter nenhuma das
três flags de fast-math; BENCH tem de conter `-O2`. Extração e guardas em
`flags.out`.

Condições de todas as execuções: 12 threads (`THREADS=12`),
`OMP_WAIT_POLICY=PASSIVE`, `run_env` único para todos os regimes, um binário
por regime (logs `build_*_<R>.log`). Host: WSL2, g++ (Debian) 14.2.0,
Intel i5-10400F — gravados em `flags.out` porque `-march=native` amarra o
resultado a esta máquina.

## Q1 — os valores do motor mudam? (`suites.out`, `values_cmp.txt`)

**Suites de igualdade exata** (`test_csr_ops`, `test_multiply_and_prune`,
normalização idêntica ao `run_all.sh`: `tail -n +2` e `grep -v Build` antes do
`md5sum`):

| suite | md5 nos 4 regimes | registro em `etapa_b/s2_close` | última linha |
|---|---|---|---|
| `test_csr_ops` | `b6a4f54ec7dc0e9c9b251254597ea215` (único) | prefixo `b6a4f54e` **IGUAL** | `ALL TESTS PASSED: 99 checks, 0 failures` |
| `test_multiply_and_prune` | `112fb48749110da3c1cf31a5612fbe91` (único) | prefixo `112fb487` **IGUAL** | `ALL TESTS PASSED` |

Ou seja: **nenhuma flag do produto mudou a saída das suites**, e os md5 batem
com os registrados (que foram gravados com as flags do `run_all.sh`, regime
BENCH — é o BENCH que valida a comparação).

**Digital de valores** (`bench_e2e`: `nnz` inteiro + `checksum` em double
sobre o resultado; variantes `dense`/`adaptive`, N = 10⁴/10⁵/10⁶, 24 execuções):

- `nnz = 2500000` **idêntico em todos os regimes, casos e N**
  (`nnz_distinto=1` em todas as 6 linhas) — a metade dura do digital, imune ao
  instrumento de ponto flutuante.
- `checksum` **idêntico em todos os regimes no caso `adaptive`**; diverge
  **só no PROD, só no caso `dense`** (`checksum_distinto=2`):

| caso | PROD | PRODC / NFM / BENCH | Δ abs. |
|---|---|---|---|
| `dense/10000` | `-618.763379` | `-618.763392` | 1,3·10⁻⁵ |
| `dense/100000` | `1587.610864` | `1587.610859` | 5·10⁻⁶ |
| `dense/1000000` | `277.889358` | `277.889357` | 1·10⁻⁶ |

Leitura (com a limitação declarada no cabeçalho do script): o `checksum` passa
por uma soma serial em double (`bench_e2e.cpp:145`) — **pelo mesmo instrumento
que o `-ffast-math` reassocia**. Divergência *apenas* no checksum, apenas no
regime que tem `-ffast-math`, com `nnz` e suites intocados, é o retrato de
reassociação aritmética, não de mudança estrutural. `PRODC` não conserta o
checksum ⇒ a causa não é a contração de FMA.

## Q2 — o pin bit-a-bit do port sobrevive? (`golden.out`)

Dois fixtures históricos, **parâmetros recuperados por medicao** (antes só
existiam na prosa da §7.2): `N=800 L=300 k=8 seed=42` (fixture do gate) e
`N=4000 L=1400 k=20 seed=42` (linha "produto, N=4000"); `B=256 limit=N` em
ambos. O golden é gravado **por cada regime** (modo `ref` do próprio
`bench_bfstree.cpp`), e o gate do repo (`gate_bfstree.sh`) roda com as flags do
regime sobrepostas — mesma fixture, mesmo pin, mesmas asserções.

| regime | `ref` N=800 | `A4` N=800 | BITFINAL `A4` | `ref` N=4000 | `A4` N=4000 | pin `-O2` | `hist` 800 / 4000 | gate |
|---|---|---|---|---|---|---|---|---|
| **PROD** | `0x7b70dfb313c2b151` | `0x0423b12779c26d3d` | **NO 0,999983** | `0xa299c0fa9515e5de` | `0xed6d982a1d98511e` | IGUAL | **IGUAL / IGUAL** | **rc=1** (`hyp suite: RESULT: 45 checks, 6 failures`) |
| **PRODC** | idêntico | `0x7829b615b6824fb1` | **NO 0,999975** | `0xd1a967056882795e` | `0x294efaa7aff3930e` | IGUAL | DIFERENTE / DIFERENTE | **rc=1** (`modo A4 nao bit-exato: BITFINAL=NO POS_EQUAL=0.999975`) |
| **NFM** | idêntico | `0x7b70dfb313c2b151` | **YES 1,000000** | `0xd1a967056882795e` | idêntico | IGUAL | DIFERENTE / DIFERENTE | **rc=0** (`BFSTREE GATES: OK`) |
| **BENCH** | idêntico | `0x7b70dfb313c2b151` | **YES 1,000000** | `0xd1a967056882795e` | idêntico | IGUAL | DIFERENTE / DIFERENTE | **rc=0** (`BFSTREE GATES: OK`) |

Além disso, `A4_repeticao_md5_unico=1` nos quatro regimes (o mesmo binário,
duas execuções, mesmo `CK_FINAL`): a divergência **não** é instabilidade de
run.

Leituras, com o que **não** é claim:

1. **`HIST_*` é esperado `IGUAL` só no PROD.** Os pins históricos
   (`0x0423b127…`, `0xa299c0fa…`) foram medidos com a string do produto
   *paraphraseada* no comentário do gate; o PROD — string real do Makefile —
   os **reproduz**, o que retrovalida aquela medição. Em NFM/BENCH o valor
   correto é o golden `-O2`, e `DIFERENTE` do pin de produto é o esperado, não
   uma falha.
2. **A contração de FMA não é a culpada (PRODC).** Com `-ffp-contract=off` o
   gate passa a hipótese (45/45 — as 6 falhas do PROD somem) mas **ainda falha
   no bit-pin** (`POS_EQUAL=0,999975`). O `-ffast-math` restante (reassociação)
   quebra o port sozinho.
3. **NFM passa o gate inteiro e é bit-exato nas duas escalas** — `ref == A4`
   em N=800 **e** N=4000, `BITFINAL=YES POS_EQUAL=1.000000` — mantendo o resto
   do build do produto (`-O3 -march=native -DNDEBUG`).
4. BENCH reproduz o pin `-O2` (`0x7b70dfb313c2b151`) e o gate completo: os
   fixtures e o pin são medíveis na malha, não só nesta auditoria.
5. O gate de PROD falha **já na hipótese** (6/45): a flag do produto move o
   port antes mesmo do pin.

## Q3 — quanto custa o regime que preserva bits? (`timing.raw`, `timing.summary`)

`T_TOTAL_MS` = F1..F9, exclusiva da leitura do arquivo; modo `A4`, reps
**intercalados** entre regimes (r1: PROD, NFM, BENCH; r2: …) para o drift de
sessão do WSL2 atingir os três braços igualmente; mediana sobre os reps.
`REPS_SYNTH=3`, `REPS_REAL=2`.

| caso | PROD | NFM | Δ vs PROD | BENCH | Δ vs PROD |
|---|---|---|---|---|---|
| **synth** (N=12000 L=1000 k=20 s7) | 46807,34 ms (n=3) | 46695,63 | **−0,2%** | 46048,42 | −1,6% |
| **real** (ALOI N=18000 L=1150 k=20 s0) | 42189,38 ms (n=2) | 35320,05 | −16,3% ⚠ | 36834,26 | −12,7% ⚠ |

⚠ **O caso `real` não é utilizável como delta**: com n=2 o `max/min−1` chega a
+9% (PROD), **+35% (NFM)** e **+44% (BENCH)** — drift de sessão WSL2, não
regime de build. A âncora defensável para a decisão da §7.4 é o **synth**: NFM
custa **−0,2%** sobre PROD, dentro do ruído (o próprio synth tem outliers de
rep — ex.: NFM 58886 ms no rep2 — por isso a mediana) ⇒ **NFM é de graça
nesta medição** e preserva os bits (Q2).

Limitação declarada em vez de omitida: subir `REPS_SYNTH=5/REPS_REAL=3`
reduziria o ruído a ~1 h de run; optou-se por documentar o drift e ancorar no
synth — reavaliar se um delta real for decisão de custo.

## Armadilhas medidas nesta auditoria (para o próximo run)

1. **Extração do verdict (corrigido).** Três padrões de `sed` não casavam com
   os artefatos que o próprio script grava: `gk()` ancorava a chave logo após o
   regime (só o primeiro campo da linha era lido); a regex de `bitfinal_A4`
   esperava rótulo `POS_EQUAL=` que `golden.out` não escreve; a de mediana
   esperava `tag R mediana=` mas o artefato é `tag R n=N mediana=`. Os três
   estão corrigidos em `prod_numerics.sh`; o `stage_verdict` deriva os deltas
   de Q3 do `timing.summary` já gravado, então **reexecutar
   `bash Source/bench/prod_numerics.sh verdict` é barato e idempotente**.
2. **`case … esac` dentro de `$( )` dentro de aspas** é realçado vazio pelo
   bash (mesma armadilha quebrava os marcadores do golden) — o script usa
   `eq()`/instrução própria, ver comentário em `stage_suites`.
3. **`-march=native` amarra PROD a esta CPU**; qualquer reprodução exige o
   mesmo `model name` (`flags.out`).
4. **Suites com igualdade exata** não percebem valor que muda sem violar
   igualdade — por isso o estágio `values` existe (alarme de asserção ×
   digital de valores).
5. **Golden do port é sintético em N=800/4000** (débito 6.4 do dossiê):
   bit-exatidão em dados reais grandes continua em aberto depois desta
   auditoria.

## Passo 0 — a numérica do MOTOR sob as duas strings de flags (LHRR/CPRR)

Pergunta (não é sobre o BFSTree, é sobre o motor): as flags do produto mudam a
saída do `multiply_and_prune`/builder v2 que CPRR, LHRR e o port usam? Driver
`Source/bench/pass0_rkcmp.sh` (protocolo herdado de `_p2_rkcmp.sh`: taskset,
`PHASE_RK_OUT` + `PHASE_DUMP_DIR`, REPS=1), ALOI `N=72000 L=400 k=10 limit=400`,
braços **PROD vs NFM** (as mesmas strings de `flags.out`), T=1 e T=12.

| pipeline | `.rk` final | dumps canônicos |
|---|---|---|
| **CPRR** | `ec88d926…` **igual** (PROD = NFM e T=1 = T=12); saída ≠ input (`76087c1f…`) | `W`, `result` **idênticos** |
| **LHRR** | **`4eb78982…` vs `68ba2066…`** — 58.558/72.000 linhas (81,3%), τ≈0,995 | `H`: estrutura igual, **88,98% dos valores a 1 ULP** (`max|delta|=1e-6`); `MM` idêntico; **`result`/`S`/`Srev`/`Stot` com `nnz` 8–11% menor no NFM (ESTRUTURAL)** |

Leitura: no LHRR a diferença de **1 ULP no `H`** muda o que o prune a jusante
descarta na fronteira → o suporte das matrizes agregadas cai (`result`:
5.709.746 → 5.248.535 entradas) → 81% das linhas do `rk` final mudam. No CPRR a
mesma troca de flags não move um bit. Determinismo está descartado como causa:
dentro de cada regime T=1 e T=12 dão md5 **idêntico** nos 4 casos.

**Consequência — decisão em aberto.** Adotar NFM no produto **não é neutro para
o LHRR** (muda suporte/`nnz` e a saída final); a numérica do motor deixa de ser
"problema só do BFSTree". O registro do `A4` em `Source/src` ficou **parado**
até a decisão entre: alvo próprio por método, NFM global (aceitando a mudança
medida acima) ou contrato de tolerância. Evidência:
`pass0_flags.txt` (as duas strings), `pass0_gate.txt` (gates + classificação),
`pass0_report.txt` (inventário completo com `dump_cmp.py` por arquivo),
`pass0_runs.out` e os 8 logs `pass0_<pipe>_t<T>_<R>.log`; `.rk`/dumps (920 MB)
fora do repo, em `/home/andre/udlf_cache/pass0/`. Estágios:
`bash Source/bench/pass0_rkcmp.sh build|runs|gates|report` (o `gates` retorna
≠0 em divergência).

### M1 — a perturbação mexe em qualidade? (não; e o NFM é ligeiramente melhor)

`pk_eval.py eval` sobre os `.rk` do LHRR (`pass0_quality.txt`); `L=400 k=10
limit=400 T=1`:

| regime | P@4 | P@10 | P@20 | MAP@100 | MAP@400 |
|---|---|---|---|---|---|
| PROD | 91,648960 | 84,200835 | 75,103194 | 45,630985 | 49,115577 |
| PRODC | 91,648960 | 84,200835 | 75,104028 | 45,695409 | 49,187452 |
| NFM | 91,648960 | 84,200835 | 75,103056 | **45,712760** | **49,203441** |

P@4/P@5/P@10 **idênticos nos três**; as métricas profundas variam ~0,1 pp e
**a favor do NFM**. Ou seja: 81% das linhas mudam de posição e a qualidade
estatisticamente não se move. Aviso declarado: é o mesmo pipeline, via
**driver**, não a config publicada do framework (uma equivalência end-to-end
com `Data/Unified_Accuracy_Results.csv` seria M4 e não foi feita).

### M2 — a causa é contração de FMA ou reassociação? (MISTA — a alternativa barata caiu)

Braço `PRODC = PROD + -ffp-contract=off` (resto do fast-math intacto), T=1/12
(`pass0_m2.txt`):

- `.rk` do LHRR: **três valores distintos** — PROD `4eb78982`, PRODC `c4c6ac73`,
  NFM `68ba2066` → o `PRODC` difere **dos dois**;
- o `PRODC` muda a estrutura de novo (`result`: 5.709.746 → 5.815.878 nnz, ainda
  longe do 5.248.535 do NFM) e o `H` continua com valores a 1 ULP (79,4% das
  entradas vs PROD);
- CPRR idêntico nos três braços (controle);
- **veredito pré-registrado: causa MISTA** ⇒ `-ffp-contract=off` sozinho **não**
  estabiliza o motor. A única medida que estabiliza estrutura **e** o pin do
  BFSTree (que falha em PRODC, `POS_EQUAL=0,999975`) é o **NFM completo**.

Consequência registrada: não existe atalho de um flag; a saída escolhida foi o
**alvo estrito separado** (ver `bfstree_close` §7.4), deixando o regime global
do produto em aberto — re-baselinar LHRR em 3 datasets é a conta em aberto.

## Arquivos

| artefato | estágio | conteúdo |
|---|---|---|
| `flags.out` | `flags` | string real do Makefile, os 4 regimes, guardas, host/compiler/CPU |
| `suites.out` + `suite_*.log` + `build_test_*` | `suites` | md5 por suite × regime vs registro `s2_close` |
| `values.digest`, `values_cmp.txt` + `e2e_*.log` | `values` | nnz + checksum, 4 regimes × 2 variantes × 3 N |
| `golden.out` + `gold_*.log` + `gate_*.log` | `golden` | pins por regime, N=800/N=4000, gate completo com mensagem |
| `timing.raw`, `timing.summary` + `time_*.log` | `timing` | 15 execuções brutas, medianas e deltas |
| `verdict.txt` | `verdict` | Q1/Q2/Q3 derivados por sed/awk dos acima |
| `run2.log` | — | transcrição da execução (`grep DONE run2.log`) |
| `pass0_flags.txt`, `pass0_gate.txt`, `pass0_report.txt`, `pass0_runs.out`, `pass0_*.log` | `pass0` | motor LHRR/CPRR (ALOI 72k) PROD vs NFM: strings, gates, inventário de dumps, 8 runs |

Reprodução: `bash Source/bench/prod_numerics.sh <estágio>` (um estágio de cada
vez é válido; `verdict` roda em segundos). O dossiê que consome estes números é
`Data/phase_profile/bfstree_close/README.md` §7.2–§7.4.