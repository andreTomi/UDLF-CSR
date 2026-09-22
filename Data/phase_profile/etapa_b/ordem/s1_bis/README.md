# S1-bis — A/B same-session com a política permanente (Fase 1, pipeline real)

Fecha o número da Fase 1 para a tese. Protocolo: `/tmp` é volátil neste WSL2 (o VM
idle-out limpa tmpfs e mata jobs em background), então o harness vive no repo
(`Source/bench/s1_bis_run.sh`), escreve em `raw/` **persistente** e é retomável
(pula etapas cujo arquivo já existe). Os dois headers vêm de **git**, não da árvore
de trabalho: `base = 86898d5` (pré-Fase 1), `head = 3c08bd1` (Fase 1 com o fix de
ordenação). Mesmo driver para ambos (o driver ganhou a linha `REPRU`).

Protocolo: 1 warm-up descartado por binário, 3 pares intercalados
(`b1 base → b2 head → b3 head → b4 base → b5 base → b6 head`), 2 reps cada,
`taskset -c 0-11`, ALOI `N=72000 L=400 k=10 limit=400 T=1`, `lhrr`; canário
(carga fixa w1, head) antes/depois.

## Resultado

| pos | kernel | rep0 | rep1 | mediana | minflt rep0 | minflt rep1 |
|---|---|---|---|---|---|---|
| 1 | base | 3795,1 | 3895,7 | 3845,4 | 881 819 | 877 012 |
| 2 | head | 3747,4 | 3260,3 | 3503,9 | 699 563 | 700 551 |
| 3 | head | 3259,2 | 3390,8 | 3325,0 | 699 558 | 700 687 |
| 4 | base | 3909,3 | 4603,9 | 4256,6 | 881 804 | 882 727 |
| 5 | base | 3880,4 | 3723,5 | 3802,0 | 881 915 | 882 606 |
| 6 | head | 3276,0 | 3429,9 | 3353,0 | 699 586 | 694 372 |

Razões head/base **sob a política** (primeiro bloco descartado; rep 0 descartado):

| leitura | base | head | razão |
|---|---|---|---|
| todos os reps | 3 845,4 | 3 353,0 | 1,147× |
| todos os reps + 1º bloco descartado | 4 029,3 | 3 353,0 | **1,202×** |
| só rep1 | 3 895,7 | 3 390,8 | 1,149× |
| só rep1 + 1º bloco descartado | 4 163,7 | 3 390,8 | **1,228×** |
| pares adjacentes (1º bloco descartado) | — | — | 1,412 / 1,358 / 1,086 → mediana **1,358×** |

Correção: base e head produzem o mesmo nnz/checksum no mesmo binário de driver
(`5 248 535 / 119 396 289 001.122040`).

## Veredito

**A Fase 1 corrigida é ~1,15-1,20× mais rápida que o kernel pré-Fase 1 no pipeline
LHRR real @12t.** Cruzando as duas sessões (S1 e S1-bis), a razão global sob
política é 1,150× (S1) e 1,202× (S1-bis); os pares adjacentes concordam em direção
em **9 de 9** (6 do S1 + 3 aqui). O intervalo defensável na tese é **1,15-1,20×**
(global, sob política) e não o 1,26× do pareamento bruto do S1 (puxado pelo bloco
contaminado).

## Duas coisas que a instrumentação resolveu (e uma que falsificou)

1. **Mecanismo por rep: FALSIFICADO.** A hipótese que levantei (rep 0 paga
   first-touch de páginas e por isso é sistematicamente mais lento) **não se
   sustenta**: os `minflt` por rep são praticamente idênticos dentro de cada bloco
   (base 881 819 vs 877 012; head 699 563 vs 700 551), e o rep 0 foi mais lento em
   apenas **3 de 6** blocos (no S1 foi 5 de 6). Ou seja: o padrão "rep 0 mais lento"
   do S1 era **ruído de sessão**, não mecanismo. A política de descartar rep 0
   permanece (é conservadora e não infla o ganho do head), mas **não** alegue
   first-touch como causa.
2. **Custo de memória do legacy: medido por falhas.** base ≈ **881,8 mil**
   minor faults/rep vs head ≈ **699,6 mil** → Δ ≈ **182 mil páginas/rep ≈ 747 MB**
   de first-touch extra por repetição (páginas de 4 KB; THP não está ativo). É a
   assinatura estrutural do `temp_triplets_`/count buffers que o builder legacy
   aloca em cada build, e explica por que o head também é mais leve.
3. **Deriva de sessão é o limite de precisão.** O canário deu **−21,5%**
   (103,1 → 80,9 ms) nesta sessão — a máquina ficou *mais rápida* com o tempo
   (oposto de throttling), o que infla a vantagem de quem roda depois. O canário de
   2 reps (~40-100 ms) é **fraco** para medir deriva; para A/B futuros usar um
   canário de ≥1 s (ex.: 1 rep do pipeline real em workload pequeno).

## Arquivos

`raw/` (warm_base, warm_head, canary_0, b1..b6, canary_1) · agregador
`Source/bench/s1_bis_agg.py` (aplica a política e imprime todas as leituras, para
que o efeito da política fique visível) · runner `Source/bench/s1_bis_run.sh`.
