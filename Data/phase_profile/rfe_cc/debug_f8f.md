# Debug F8f (Etapa 6) -- hipóteses e resultados

Protocolo: uma hipótese por vez, gated pelo parity gate
`Source/tests/test_rfe_cc_parity.cpp` (build WSL, `-O2 -ffp-contract=off`).

## H20 -- buffer indexado por label mas dimensionado por `ncc`
- **Hipótese**: CSR de `embycc` (`emb_by_cc_sparse_`) era dimensionado por
  `ncc` (nº de CCs com >1 membro) mas indexado pelo label ORIGINAL do CC,
  causando overflow/aliasing (padrão B1).
- **Estado**: já mitigado no código atual via `label_to_idx_` (mapeia label
  original -> índice denso 0..ncc-1) + `inv_ccemb_` invertido. Não reproduz.

## H21 -- `static thread_local htab` quebrando o acumulador de hash
- **Hipótese**: `htab` com `static thread_local` em f8e/f8f reutilizava estado
  entre casos/chamadas (padrão B2).
- **Estado**: não há `static thread_local` no `RFECore.hpp` atual (verificado
  por grep). Não reproduz.

## H22 -- zero-check causando visitas duplicadas / skip indevido
- **Hipótese**: filtrar `v != 0` antes de acumular mudaria o somatório (B3).
- **Resultado**: o gate exige bit-igualdade; o caminho atual NÃO filtra sinais
  no Gustavson (comenta "NO sign filter"). ok.

## D4 -- retorno silencioso em `ncc == 0`
- **Hipótese**: `f8f_compute_embs_by_ccs` retornava cedo sem zerar matrizes.
- **Estado**: `if (ncc == 0) return;` mantido, mas gate cobre fixtures com
  CCs de >1 membro; degen fixture (cc_degenerate) verde.

## H23 -- `f8b_init_th_cc` sem o guarda `if (th_cc == 0)`  ***RAIZ 1***
- **Sintoma**: caso 1 (th=0.0) verde em TODAS as fases; todos os casos com
  th>0 (0.05/0.5/1.0/2.0) falhavam em `F8b: th_cc bit-identical` e CASCADE
  para F8b prefix -> F8d icc/rcc -> F8e ccemb -> F8f embycc/simEmb.
- **Causa**: `RFECore::f8b_init_th_cc()` SEMPRE recalculava
  `th_cc = avg/2`, sobrescrevendo o input fixo. O legado
  (`RFE::initEdgesCCs`, RFE.cpp:348) só calcula quando `th_cc == 0`
  (`rfe_ref_verbatim.hpp:867` tem o guarda).
- **Correção**: guarda `if (th_cc_ == 0)` adicionado (RFECore.hpp
  `f8b_init_th_cc`), espelhando o legado byte a byte (float accumulation).
- **Resultado**: F8b verde em todos os 14 casos; cascata resolve.

## H24 -- `validate_input` (H4) perdeu a resolução H19 opção (a) no
##        `git checkout` do incidente de corrupção  ***RAIZ 2***
- **Sintoma**: após H23, teste passava os 9 primeiros casos e ABORTAVA no
  10º (`cc_selfoff_early`) com
  `what(): RFE: row 0 does not have self at index 0`.
- **Causa**: o `git checkout Source/src/Methods/hpp/RFECore.hpp` (feito para
  restaurar o arquivo corrompido pelos scripts Python) restaurou o HEAD
  com o H4 estrito ("self no índice 0"), que é anterior à resolução H19
  (opção (a), não commitada). Só o membro `self_off0_` sobreviveu; o check
  "linha CONTÉM o próprio id em qualquer posição", o contador e o log
  `RKSELF_OFF0` tinham sido perdidos.
- **Correção**: `validate_input` restaurado para a variante H19 (a):
  - recusas duras mantidas: id fora de `[0,n)`, duplicado, self AUSENTE
    (mensagem "does not contain its own id");
  - self fora do índice 0 ACEITO + contador `self_off0_` +
    log `RKSELF_OFF0 rows=N off0=M (accepted; legacy pins self during the F1 sort)`;
  - accessor `self_off0()` re-adicionado (exigido por
    `test_rfe_selfoff0.cpp:135`).
- **Resultado**: casos 10-12 (selfoff) e 13-14 (cíclico) verdes.

## H25 -- variante densa (A) de `embycc` inexistente
- **Sintoma**: `f8f_compute_embs_by_ccs` com `f8_embycc_sparse_ == false`
  lançava `"dense emb_by_cc not implemented"`.
- **Correção**: Variant A implementada (merge-join `simEmb(emb[q], ccemb[lbl])`
  direto, n x n, `emb_by_cc_dense_`), `embycc_get` e `sim_embycc` com caminho
  denso correspondente. Gate continua rodando a Variant B (CSR) como padrão;
  A disponível para benchmark.

## Resultado final (2026-10-09)
- `test_rfe_cc_parity`: **6144 ok / 0 FAIL**, 14 casos
  (`RFE CC PARITY (Etapas 2-6): OK`). F8f[B-csr] verde nos 14 casos:
  embycc bit-igual, H16 (não-negativo/finito), simEmb(embycc) bit-igual.
- Prints `[DEBUG ...]` removidos do core e do gate (grep `DEBUG` = 0 hits).
- `run_all.sh` completo: **exit 0** (todas as portas verdes, incluindo
  golden CC, parity CC Etapas 2-6 e self-off0 H19).
- ASan+UBSan do gate CC (OMP=4): **0** `AddressSanitizer`/`runtime error`,
  transcripto verde.
- Gate renomeado para "Etapas 2-6" (test + run_all.sh).
- Arquivos de debug descartados (`tmp_*.py`, `fix_f8*_debug.py`,
  `test_cc_structured.cpp`, `repro_f8f.cpp`, `check_access.cpp`).
