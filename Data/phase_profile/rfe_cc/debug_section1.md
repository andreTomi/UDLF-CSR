# Section 1 (H27) -- guarda provisório do PERFORM_CCS

Data: 09/10/2026. Base: `63f2f2e` (Etapas 1-6, árvore limpa antes da edição).

## H27 -- `PERFORM_CCS` era aceito e IGNORADO silenciosamente (CONFIRMADA)

**Sintoma.** `RFECore` construtor (`RFECore.hpp:68-86`) aceitava
`performCCs` mas só fazia `if (performCCs) th_cc_ = th_cc;` -- redundante
com a init-list `: th_cc_(th_cc)` (linha 70). Não havia membro
`performCCs_`; `run()` (1074+) não tocava o flag. Logo, os 7 `.ini`
(vegfru/sun397/cifar/corel5k/aloi/cub200/linnaeus5, todos
`PARAM_RFE_PERFORM_CCS = TRUE`) rodavam `bin/udlf_strict` e devolviam
rkLists SEM CC, sem erro. O comentário de `run()` ("REFUSES those configs at
construction") era falso; o `_backups/audit_freeze/RFECore.hpp:80-84` prova
que houve um `throw "PERFORM_CCS not ported yet"` que foi removido.

**Correção (provisória, ate a Etapa 9).**
- `performCCs_` guardado no ctor; o `if` redundante removido.
- `run()` lanca no TOPO (antes de `validate_input`, para recusar cedo):
  `"RFE: PERFORM_CCS em integracao, ver Etapa 9 ..."`.
- Carrier `RFE.hpp:123`: log "...PERFORM_CCS supported" (falso) corrigido
  para "...NOT yet honoured -- refused in run(), Etapa 9".
- Comentários falsos do ctor e de `run()` reescritos.
- Aceite da Etapa 9: remover o `throw` + `grep "Etapa 9"` = 0 em
  `RFECore.hpp`.

**Risco para os gates (verificado: nenhum).** Nenhum teste constrói
`RFECore` com `performCCs=true`; `test_rfe_cc_parity.cpp:103` e
`test_rfe_selfoff0.cpp:81` usam o default `false`. Confirmado por grep.

**Gates após a mudança (todos verdes).**
- `test_rfe_cc_parity` (Etapas 2-6): 6144 ok / 0 FAIL.
- `test_rfe_selfoff0` (H19 + novo teste): 0 FAIL; as duas novas asserções
  ("refuses performCCs=true" e "performCCs=false roda sem throw") PASSAM.
- `test_rfe_parity` (F1-F7b): OK (pin `ck_rk=0x3465e556d414ca79`).
- `test_rfe_cc_golden`: OK.

**Novo teste** em `test_rfe_selfoff0.cpp` (surface 1b): `run()` com
`performCCs=true` tem de lançar com "Etapa 9"; controlo `performCCs=false`
na mesma entrada valida tem de correr sem throw.

## Premissas do plano NAO confirmadas (cortadas, com prova)

1. **`n + 1000`** -- grep `n + 1000|n_ + 1000|\+ 1000` em 743 ficheiros:
   **0 resultados** em `RFECore.hpp`. Item cortado.
2. **`assert` sem include** -- grep `assert(` em `RFECore.hpp`: **0**. Os
   asserts existem em `SparseMatrix.hpp:390-391` (protegidos por
   `#ifndef NDEBUG`) e `test_rdpac_ref`, não no core. Item cortado.
3. **restaurar `<set>`** -- `RFECore.hpp` **nao usa `std::set`** (grep: 0);
   o `set_difference` de `f7b` usa `std::vector`. Apenas `<cstdio>` e que
   falta (usa-se `std::printf` em `RFE.hpp:1056`/RKSELF_OFF0). `<set>` sera
   adicionado so se o F8g o exigir.
4. **"Reative o throw de RERANK_BY_EMB/EXPORT_EMBEDDINGS" (Etapa 9)** -- o
   throw **ja esta ativo** em `RFE.hpp:92-96`. Nada a reativar; so
   confirmar/commentar.

## Confirmado e adiado para as Etapas proprias

- **Truncagem do `f8c`**: a referencia `processCCs` (`rfe_ref_verbatim.hpp:
  849-862`) NAO trunca `edgesCCs_` -- lehe por valor e `break`. O
  `edges_ccs_.resize(m)` do core (`RFECore.hpp:2027`) e uma divergencia de
  estado observavel (o accessor devolve vetor menor). O épsilon
  (`ceil(prod - 1e-12)`, linha 2024) diverge em `bound=34.00000000000001`
  (legado 35, épsilon 34). Ambos ficam para a **Etapa 6b item 3**; ai o
  prefixo passa a ser contagem guardada (nao resize) e `f8d` recebe `m`.
- **`f8a`-`f8f` sem `inline`** (1946/1996/2018/2031/2061/2166): ODR latente
  se 2 TUs incluirem o header no mesmo binario. **Etapa 6b item 1**.
- **Guard `ncc == 0` morto** + comentário "non-singleton" errado (2167):
  `label_list` conta todas as raízes, singletons incluidos. **Etapa 6b
  item 2** (com gating: excluir singletons muda `label_to_idx_`/`ncols`).
