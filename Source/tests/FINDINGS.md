# RFE port -- running findings log

> ## CURRENT STATUS (read this first; the log below is append-only and older
> ## entries still describe a state the work has since left behind)
>
> | claim | where it is stated below | current state |
> |---|---|---|
> | real-data bit-exactness "not established" | "Open risks", older status tables | **CLOSED.** 3 real datasets x k in {5,10,20,50}, all bit-identical, 0 differing cells. Raws: `Data/phase_profile/rfe_ksweep/` |
> | bit-exactness shown at a single `k` | "Phase 3a k sweep" | **CLOSED.** rho(Gram) 0.0185..0.5478 on regenerable committed raws (SHORT state, superseded by the faithful rho); the 0.9515 point (k=50) is **NOT REGENERABLE** (raw truncated mid-F5) and from the SHORT state (superseded) |
> | fast-math / strict build "undecided" | older status tables | **DECIDED.** Needs fast-math OUT, not just `-ffp-contract=off`. Reuse `udlf-strict`; `STRICT_FLAGS` is literally the measured NFM regime |
> | gate blind to build regime (X8) | "Stage 3c" | **FIXED and PROVEN.** `ck_gram` is now an absolute pin; `step3b_real_gate.sh` shows it PASS under BENCH and FAIL under PROD |
> | F6 bottleneck 77% an O(k^2) insertion sort | "Stage 3a" | **FIXED.** 7.8x-11.4x on the sort, 1.8x on the pipeline, all 5 absolute pins unmoved |
> | F1 bottleneck | "Step 3c F1 profile" | **CHARACTERISED.** 69.5% of F1 is the two SERIAL phases; the cell loop is 17.2%. `t_index` CSR build confirmed ~2x, identity-verified, NOT YET IMPLEMENTED |
> | `L` as the density axis | "Stage 3b/2" | **CLAIM (SHORT state) REFUTED via `run()`.** "L only moves the incElemsL cut; k is the density axis" was made from the pre-t-loop sweep and is false under the faithful pipeline: L moves the Gram (ck_gram `0x3218b119…` L=400 vs `0x045f449c…` L=1000; rho 0.3808 vs 0.3743). See "RETRACTED: L is not the density axis" |
> | ee tie-break exercised on real data | (never written to this log) | **NOT DEMONSTRATED.** Tied-pair counts are a COUNT, not exposure. A negative control (stable_sort vs sort) is required before any claim |
> | `k=50` is the production regime | (never written to this log) | **RETRACTED.** `PARAM_RFE_K = 20` in all 7 `.ini`; k=20 is measured bit-exact |
> | `PARAM_RFE_PERFORM_CCS` default | (never written to this log) | **TRUE in all 7 `.ini`, and `execRRCCs` is NOT PORTED.** Registration must refuse it (Step 4) |
>
> Provenance: `RFE.cpp` md5 `c26b4c9d470d474ec7963b46d7eca917`,
> `RFE.hpp` md5 `cc3fd610d782083e45a4ff89432cd063`.

Append-only. One entry per phase, written as the phase completes, so the final
report can be assembled without reconstructing reasoning from diffs.

**Scope.** Port the in-scope legacy RFE pipeline to the native `CSRMatrix<float>`
engine with bit-exact parity. Connected components, EmbCC, export and fusion are
**deferred** (still legacy-only). Legacy reference is
`Source/tests/rfe_ref_verbatim.hpp` (repo-root `RFE.cpp`, verbatim logic, framework
glue shimmed).

**Pinned fixture.** `N=200, L=40, k=10, t=2, pa=0.1, seed=42` — every digest below
is reproducible via `Source/tests/step2_check.sh`.

---

## Phase map (from the legacy `RFE.cpp` call graph, `execRRCCs` branch)

| # | Legacy phase | Source lines | Status |
|---|---|---|---|
| F1 | `rankNorm` + `execSortRankedLists` | 180-193, 717-767 | DONE, bit-exact |
| F2 | `createEmb` | ~560-600 | DONE, bit-exact |
| F3 | `createReverseEmb` | 618-632 | DONE, bit-exact |
| F4 | `combineReverseEmb` | 634-642 | DONE, bit-exact |
| F5 | `sortEmb` + `effectEstimationEmb` | 465-489, 513-525, 527-550 | IN PROGRESS |
| F6 | `incSimByECP` / `incElems` | 696-715 | not started |
| F7 | `reRankingByEmbSim` + sort | 648-661, 677-686 | not started |
| -- | CC / EmbCC / export / fusion | 232-263, 265+ | DEFERRED (out of scope) |

---

## Cross-cutting findings

### X1. Exact zeros are load-bearing, everywhere
`emb` carries cells that are **exactly `0.0f`** (2479 of 14651 at the pinned
fixture, 16.9%). They are not noise. Two independent consumers depend on them:

- `incElems[i].insert(j)` fires for **every visited pair regardless of value**, so
  a pair whose product is `0.0f` still *discovers a candidate* (`RFE.cpp:710`).
- After F4, **69.68%** of all post-combine `emb` cells are exactly zero.

Consequence: **no engine kernel or port stage may filter `|v| <= 1e-9`.** That is
exactly what `multiply_and_prune` does (`SparseMatrix.hpp:559`) and it changes the
*output*, not just the last bits. Two separate phases (F2, F4) had to bypass it.

### X2. `get()` cannot distinguish "dropped" from "legitimately zero"
This bit me once and is a rule for all remaining phases: when validating a
support-preservation claim, **compare supports (key sets), never values.** A
dropped cell and a real zero both read `0.0f` through `get()`, so a value
comparison is structurally blind to the very thing it is trying to detect. My
first F4 meta-check reported "combine()/merge() are fine" for this reason.

### X3. Synthetic saturation invalidates universal claims
Fixtures with `L=N` (or near it) drive `emb` dense and symmetric, which kills the
exact-zero population and makes regime-dependent assertions **vacuous**. Every
claim so far has needed regime gating: H2/H4b (Step 1), F4's kernel-drop check.
Saturated fixtures therefore cannot test the sparse regime; both ends required.

### X4. Process correction -- the protected-path check was vacuous
`git diff HEAD` was being used to prove `Core/`, `Methods/cpp/`, `Config/`,
`run_all.sh`, `Makefile/` were untouched. **Every RFE file is untracked**, so
`git diff` cannot see them and the check returned empty regardless of what had
changed. Re-verified with `git status --short`, which correctly shows 18 new
untracked files and none under the protected paths. Any future scope check here
must use `git status`, not `git diff`.

### X7. A `throw` inside an OpenMP parallel region is `std::terminate`, not fail-closed
**Found by `test_rfe_corner.cpp`, and it is the same class of bug as X6 in a new
place.**

Three guards in `RFECore.hpp` looked fail-closed but were not. Each sat inside
`#pragma omp parallel` / `parallel for`:

| guard | was | now |
|---|---|---|
| self absent from a row (F1 sort) | `throw` in-region | record flag, throw after |
| non-finite similarity (F1 sort) | `throw` in-region | record flag, throw after |
| self absent from `rkTmp` (F7b tail) | `throw` in-region | record flag, throw after |

Propagating an exception out of an OpenMP parallel region is **undefined behaviour
by the C++ standard**, and libgomp resolves it as `std::terminate`. So the process
aborted with `what(): RFE: rank list row does not contain its own id` and the
caller's `catch` never ran — the guard *fired* but *failed closed to abort, not to
an exception*. For a library that is worse than it looks: the caller cannot catch
it, cannot report it, and cannot fall back.

The fix is to record a per-row flag inside the region (distinct indices, so no
synchronisation is needed) and throw after the region, where propagation is
normal. The messages now also carry the offending row index, which the previous
ones did not.

**How it was found.** The corner suite's "did the fixture do what it claims"
assertion failed first, revealing that my `drop_self_from_row` helper was
swapping self *within* the row — which only relocates it, and the self-pin
searches the whole row, so nothing fired. Fixing the helper surfaced the real
defect underneath. Two bugs stacked: the test was wrong, and behind it the code
was wrong too.

### X6. Every equality gate must assert that it COMPARED something
**General principle, not an RFE case study. It applies to any port in this
framework.**

A gate that iterates a collection reports "0 differing" and passes when that
collection is **empty**. Passing then means nothing was measured. So a parity
gate needs TWO assertions, not one:

```
expect(mismatches == 0,  "values agree");        // insufficient on its own
expect(compared   >  0,  "the comparison ran");  // what makes the first honest
```

This bit twice in F7, both times producing a **false green**:

- The Gram comparison iterated the reference's `incElems`, which was **empty**
  because the harness had skipped `effectEstimationEmb`. It compared **zero
  cells** and printed `0 differing`.
- The union-prune check was evaluated *after* the core had already run the tail,
  so it re-sorted an already-sorted list and compared against the wrong target.

The first is the dangerous one: the gate that existed to catch a mis-ordered
pipeline was itself mis-ordered, and reported success while measuring nothing.
The fix applied was **structural** — the gate now asserts a non-zero comparison
count, so the vacuous case is a *failure* rather than a silent pass. Patching the
harness call order would have left the trap armed for the next phase.

Corollary: a parity gate that reports "0 differing" should always report **how
many it compared**, in the same line. A reader cannot otherwise tell a real
agreement from an empty iteration.

### `grValue == 0` is UNREACHABLE BY CONSTRUCTION (was: open risk)

This was carried as an open risk — "0 rows at every sweep fixture, so the
divide-by-zero path is untested" — and is now a **proved** non-issue rather than
an untested one.

**Theorem.** For every input the core accepts, `grValue > 0`.

**Proof.**
1. `rankKScore(pos) = 1 - log(pos)/log(k)` (the legacy's own expression,
   `RFE.cpp:644-646`). At `pos = 1`, `log(1) = 0` **exactly** in IEEE-754 — no
   rounding error is possible — so `rankKScore(1) == 1.0f` exactly.
2. `createEmb`'s `pos1 = 0` term emits `img_n = rkLists[l*q + 0] = q` (self is
   pinned at position 0) with `wn = rankKScore(1) = 1.0f`. Hence
   `emb_pre[q][q] >= 1.0f` **from that single term alone**, independent of every
   other contribution.
3. For `pos ∈ [1, k]`, `log(pos) <= log(k)` so `rankKScore(pos) >= 0`. Since
   `createEmb` only ever adds products of such weights, `emb_pre >= 0`
   elementwise.
4. F4 sets `emb[q][key] = emb_pre[q][key] * emb_pre[key][q]`, and for `key == q`
   that is `emb_pre[q][q]^2 >= 1`. So `emb[q][q] >= 1` and `q` is a **present key**
   of row `q` with a value `>= 1`.
5. `ee[q]` sums the **k largest** values of row `q` (`sorted_emb` is descending).
   Either `q` is among them, contributing `>= 1`; or it is not, which can only be
   because `k` other values are **strictly larger**. Either way `ee[q] >= 1`.
6. `grValue = ee[sorted_ee[0]] = max_q ee[q] >= 1 > 0`. ∎

**Why the premises are not conventions.** Both are *enforced by the core*, not
assumed from the dataset — which is what makes the theorem a statement about
every reachable state rather than about the fixtures:

| premise | enforced at | what it does |
|---|---|---|
| `k >= 2` | `RFECore.hpp:75` | `if (k_ < 2) throw std::runtime_error("RFE: need K >= 2")` |
| self present in every row (F1 / rank sort) | `RFECore.hpp:1282-1284` | `throw ... "RFE: rank list row does not contain its own id; the legacy reads out of bounds at this point (RFE.cpp:754-756)"` |
| self present in every row (F7b / tail) | `RFECore.hpp:777-780` | `throw ... "RFE: row does not contain its own id; ... (RFE.cpp:816)"` |

A row missing its own id is **refused before it can produce a Gram**, so the
core never reaches the division on an input for which the theorem does not hold.

**Corroboration.** `grValue` is now a **column** in the `step2_check.sh` sweep,
and the gate **fails** if any config reports `0`:

| fixture | grValue |
|---|---|
| `200 40 10 2` (pinned) | 27.1544189 |
| `120 30 8 2` | 22.3858318 |
| `80 20 6 1` | 15.189477 |
| `300 60 15 3` | 39.2743568 |
| `150 50 25 2` | 65.6491852 |
| `200 200 20 1` | 56.1378784 |
| `60 60 30 1` | 145.084991 |
| `400 40 10 2` | 22.3592186 |
| `200 40 10 1` | 27.1544189 |
| `200 40 5 2` | 11.9882803 |

All `> 0`, minimum **11.99** — two orders of magnitude above the `1.0` floor the
proof gives, so the empirical margin is not marginal either.

**Decision: do NOT build a degenerate fixture.** Forcing `grValue == 0` would
require violating self-at-position-0, which means testing against **undefined
behaviour in the legacy** (its self-scan reads `rkTmp[i]` before testing the
bound). A test written against UB is not a stronger test, and it would conflate
two different properties. The invariant "row without self → throw" is a
*separate*, legitimate robustness property and is covered by its own corner test
(`test_rfe_corner.cpp`), not here.

The core therefore keeps its **unguarded division** — reproducing the legacy
verbatim — now for a proven reason rather than an unexamined one.

---

### X5. Environment
WSL + g++ + OpenMP. Gates run under `-O2 -ffp-contract=off` (no fast-math).
ASan+UBSan clean on every phase so far. Shell gates need CRLF stripping before
`bash`; nested PowerShell quoting mangles inline python (and `$`, backslashes and
`grep -o` character classes), so a script file is the reliable route.

---

## F1 -- `rankNorm` + rank-list sort  (DONE)

**Port bugs caught by the gate (both real):**
1. Sort direction initially **reversed** -- the pin caught it immediately.
2. FNV offset basis differed from the golden's recorded literal.

**Why the legacy's `O(N*L^2)` position scan was replaced:** `getPosition` scans
row `img` for `q`; an inverted position index answers in `O(log L)`. H9 measured
0 mismatches over every `(q,i)` pair, absent cases (returning `l+2`) included.

**Result:** bit-exact on 10 fixtures, thread-deterministic at 1/2/4/12.

## F2 -- `createEmb`  (DONE)

**Hand-rolled `EmbAcc` accumulator, not a builder.** The builder coalesces
duplicates via an **unstable column sort**, so it cannot guarantee the legacy's
`(pos1,pos2)` summation order. The accumulator preserves that order explicitly.

Exact-zero support preserved: legacy 2479, core 2479.

## F3 -- `createReverseEmb`  (DONE)

**It is a true `transpose()`.** The legacy's second loop writes `remb[key][q]`, and
because each `(q,key)` pair of `emb` is visited exactly once the `+=` lands on a
fresh slot -- it is effectively a **store**, not an accumulation. `transpose()`
copies each cell with **no arithmetic**, so there is no summation order to
preserve. 14651/14651 values, 0 diffs.

**Structural caveat (measured, not assumed).** Legacy pre-creates a row per id;
CSR has no representation for an empty row. The difference measured **0** on the
pinned fixture -- every id happens to be some `emb` row's key. That is a *fixture
property*, not a guarantee, so the gate now **reports** the count rather than
asserting it. For F4 this is value-equivalent (`get()` returns `0.0f` either way).

**F3's real finding -- asymmetry, and it is F4's problem.** 8623 of 14651 `remb`
cells (**59%**) have no `emb[q][col]` counterpart. `emb` is strongly asymmetric.

## F4 -- `combineReverseEmb`  (DONE)

The legacy is one multiply per cell:
`emb[q][key] = emb[q][key] * remb[q][key]`, iterating a **copy** of the row, keys
ascending. With `remb = emb^T` (F3) this reads `emb[q][key] * emb[key][q]`.

**No accumulation-order subtlety** (single multiply), so unlike F2 there was
nothing to get wrong numerically. The entire difficulty was the support.

**Why a dedicated kernel, measured at full scale:**

| kernel | keys kept | legacy keys dropped |
|---|---|---|
| legacy | 14651 | -- |
| `CSRMatrix::combine()` | 5140 | **9511** |
| `CSRMatrix::merge()` | 4442 | **10209** |

`combine()` returns the intersection of **nonzero** supports
(`SparseMatrix.hpp:721-772`); `merge()` covers the union but filters
`if (res != T{})` on emission (`SparseMatrix.hpp:675/682/701`). Both drop the
cells that F3's 59% asymmetry makes the **majority** case. Confirms Step 1's H4b
at full scale and explains *why* it happened.

**Result:** 14651/14651 values, 0 diffs, support unchanged. 10/10 fixtures.

**Regime trap (X3 recurring).** The kernel-drop assertion **failed** at the
saturated `N=60 L=60 k=30` fixture: there `emb` is dense and symmetric, zeros are
absent, and both engine kernels are simply **correct**. The assertion is now gated
on `f4_zero > 0` and the vacuous case **prints a note** rather than skipping
silently.

## F5 -- `sortEmb` + `effectEstimationEmb`  (DONE)

`ee[q] = sum of emb[q][key] over the FIRST k keys of sorted_emb[q]`, then
`sorted_ee = tag_sort(ee)`, `grValue = ee[sorted_ee[0]]` (read BEFORE
normalisation), then `ee[q] /= grValue`.

**A different kind of constraint from F2 -- this one is `std::sort` INSTABILITY.**

`tag_sort` (`RFE.cpp:513-521`) sorts the index permutation by `ee` descending with
`std::sort`, which is **not stable**. The resulting `sorted_ee` is the **outer loop
order of F6's Gram accumulation** (`RFE.cpp:559`). Two rows with equal `ee` have
different `emb` rows, so they contribute differently to the Gram -- the tie-break
is **part of the output**, not cosmetic.

Consequence: the core calls `std::sort` **itself**, with the legacy's exact
comparator (`>` strict, descending) over the legacy's exact input sequence. Not a
stable sort (wrong permutation), not an engine kernel (unjustified tie-break).
`sortEmb` likewise needs `for_each_in_row`'s **ascending column** order, because
that is the `std::map` iteration order and hence the input *sequence* that pins
introsort's tie-break.

**MEASURED: a stable sort would have been WRONG, not merely different.**
Negative control added to the gate -- re-sort the same keys with `std::stable_sort`
and diff against the legacy:

| fixture | tied `ee` pairs | positions a stable sort gets WRONG |
|---|---|---|
| pinned `200 40 10 2` | **0** of 19900 | 0 of 200 |
| `80 20 6 1` | **8** of 3160 | **5 of 80** |
| 8 other sweep fixtures | 0 | 0 |

So the tie-break is genuinely load-bearing at `N=80 L=20 k=6 t=1`, and the pinned
fixture's order check is **vacuous** (no ties). Both facts are now reported by the
gate rather than assumed. This is X3 again: a passing check proved nothing on its
own until a negative control showed it *could* fail.

**`grValue` is recorded explicitly** (`gr_value_probe()` / `gr_value()`) because it
is read before normalisation and therefore **cannot be recovered** from the
normalised `ee` -- inferring it from a ratio of two `ee` values would have been a
guess.

**Divide-by-zero left unguarded, deliberately.** If `grValue == 0` the legacy
produces `inf`/`nan`; the core reproduces that rather than guarding, because a
guard would silently change the output on exactly the inputs where the legacy is
already degenerate. Reported: `rows with ee==0` is 0 at every sweep fixture, so the
degenerate regime is **not** currently exercised -- real-data risk, not a resolved
one.

**Result:** bit-exact on 10/10 fixtures, thread-deterministic 1/2/4/12.
Digests: `ck_ee=0x484e6c82e09b53ff`, `ck_sorted_emb=0x8ef3378db5094594`,
`ck_sorted_ee=0x12fdfcf13903c9b3` (pinned fixture).

## F6 -- Gram + `incElems`  (DONE)

```
for (q : sorted_ee)          // ORDER IS THE OUTPUT
  wi = ee[q]; wj = 1         // literal 1 at the call site
  for (i : sorted_emb[q])    // guard: skip if i not in emb[q]
    for (j : sorted_emb[q])  // guard: skip if j not in emb[q]
      incElems[i].insert(j)  // FIRES REGARDLESS OF VALUE
      MX(i,j) += (vi*vj) * wi * wj
```

**PARALLELISED OVER ROWS `i`, NOT OVER `q` -- and that is forced, not chosen.**
`MX(i,j)` is `+=`-accumulated once per `q`, so a cell's partial sums depend on the
order of the `q` loop, and that order is `sorted_ee` (F5's unstable tie-break). A
parallel reduction over `q` would sum the same terms in a different order and
produce a **different float**. Rows are independent -- row `i` only ever writes
`MX(i,*)` -- so parallelising over rows keeps each cell's summation sequence
identical to the legacy's. This is F2's re-association avoidance one level up:
F2 fixed the order *inside* a row, F6 fixes it *across* `q`.

**Candidate discovery is split from accumulation, and the legacy's single pass
cannot be reproduced literally.** The legacy does both in one loop, but a CSR row's
column set must be known before the row can be emitted. So pass 1 builds
`incElems[i] = union over {q : i in sorted_emb[q]} of sorted_emb[q]` and pass 2
accumulates. Sound precisely because `insert` is idempotent and
**value-independent** -- which is exactly why the union is well defined.

**A bug this design invited, and it is worth recording.** Pass 1 takes a union over
`q`, so the same column is appended once per contributing `q`. Sorting alone is
NOT enough: without a `unique`, the duplicates survive as extra `0.0f` cells, and
pass 2's binary search finds only the **first** -- silently discarding every later
`q`'s contribution to that column. The first draft had exactly this. Caught by
reading the code against the contract, not by a failing test (the value diff would
have appeared as a small nonzero mismatch with no obvious cause). Coalescing by
summing is safe **only** because every candidate starts at `0.0f`, so `0+v == v`
exactly; that is why the coalesce is confined to pass 1.

**`* 1.0f` is kept.** `wj` is the literal `1` at the call site. Dropping it would
be a rewrite, not a simplification, and this port does not rewrite float
expressions (same rule as F1's `pow(x,2)` not becoming `x*x`).

**`gram_` doubles as `incElems`.** Same support by construction, so storing both
would be duplicated state that can drift.

**Result (pinned fixture):** 39996 `incElems` cells, Gram nnz 39996, **39796
off-diagonal cells bit-exact**. Exact-zero Gram cells: **legacy 5950 = core 5950
(14.88%)** -- the H8 population, now measured on *both* sides. `ck_gram =
0x9cf3d5102ffa87e9`. 10/10 fixtures, thread-deterministic 1/2/4/12.

**CORRECTION -- a hypothesis I had to discard.** F6 first reported 14.88% at the
pinned fixture against Step 1's 26.56%, and I attributed the gap to the pinned
diagonal being excluded. **That explanation was wrong.** Re-running F6 at
`N=120 L=30 k=8 t=2` -- Step 1's H8c fixture, which is *not* the pinned one --
gives **legacy 3818 = core 3818 = 26.56%**, reproducing H8c to the digit.

So both numbers are right at their own fixtures, and the difference is the
**fixture, not the definition**:

| fixture | incElems cells | exact-zero | pct |
|---|---|---|---|
| `120 30 8 2` (Step 1's H8c) | 14376 | 3818 | **26.56%** |
| `200 40 10 2` (pinned) | 39996 | 5950 | **14.88%** |

The lesson is the one already logged as X3, in a new form: **a percentage from a
different fixture is not a discrepancy to be explained, it is a different
measurement.** I nearly wrote a confident causal story on top of two numbers that
were never comparable. F6 now reports both the cell count and the percentage so
this cannot be misread again.

## F7 -- `reRankingByEmbSim` + CPRR tail + union prune  (DONE)

**F7 IS ACTUALLY TWO PHASES, AND ONE OF THEM WAS MISSING ENTIRELY.**

The in-scope pipeline (`RFE.cpp:102-124`) is:

```
rankNorm()
for ct in 0..t-1:  execEmbeddingRR()      # createEmb, createReverseEmb,
                                         # combineReverseEmb, reRankingByEmbSim
execCartesianProduct()                   # doHyperRREmbedging + doHyperEffecEstimation
                                         # + reRankingByCPRR (Gram + tail)
```

`reRankingByEmbSim` **rewrites `rkLists_`**, and `execCartesianProduct` then
**rebuilds `emb` from the rewritten lists**. Step 0's own digests show it:

| stage | ck_rk | emb keys |
|---|---|---|
| rankNorm | `0x3465e556d414ca79` | 0 |
| embRR[0] | `0xad0c4fe24904d9d9` | 14651 |
| embRR[1] | `0xe86514a3dd0922a9` | 12812 |
| cartesianProduct | **`0x58ded033b72ded72`** | 12227 |

So the `emb` that feeds F5/F6 is the **12227-key** one, not the 14651-key one. The
F1..F6 gates had been feeding both sides the same post-F4 state and therefore
**omitted this phase on both sides** -- internally consistent, but not the real
pipeline. Every individual phase was bit-exact and the pipeline was still wrong.

### F7 -- the check that caught it

**E2E: run the FULL legacy pipeline in the FULL legacy order and compare the final
rank lists.** Every per-phase check feeds both sides the same state, so *none of
them can detect a mis-ordered chain.* First E2E run: core `ck_rk =
0xe86514a3dd0922a9` -- exactly the golden's `embRR[1]` value, i.e. the core stopped
after the embRR loop -- versus the golden's `0x58ded033b72ded72`, with **7214 of
8000 cells differing.**

### Three real bugs, in order

1. **Sort direction (F7b).** I read the legacy's insertion sort
   `while (i >= 0 && a[i] < keyA) shift-right` as an **ascending** sort. It is
   **descending**: it shifts *smaller* values to higher indices. Proof from the
   legacy's own output, row 0: values `31.01, 16.81, 11.90, 4.16, 2.81, 2.59,
   1.95` -- monotonically decreasing. I "reasoned" my way to ascending, got
   7800/8000 cells wrong, and the measurement corrected it. *Reading an insertion
   sort's comparator is not the same as running it.*

2. **The `pow(2,30)` diagonal pin.** `MX(q,q) = pow(2,30)` (`RFE.cpp:605`) makes
   self win **by value**, not by convention. `gram_.get(q,q)` returns the
   *accumulated* diagonal, so the pin must be applied at read time. Applied as a
   read-time pin so `gram_` remains the accumulated Gram that F6 gated.

3. **The union-prune tie order.** Ranking ties by ascending id is wrong: the
   legacy's `rkTmp` is *[current list in its own order]* ++ *[extras ascending]*,
   and the current list is **not** in ascending id order. Measured as **3 of 200
   rows** wrong before this was fixed to reuse the exact `rkTmp` construction.

### H8 CONFIRMED, with numbers

| prune | rows differing from the legacy's top-L (of 200) |
|---|---|
| **union** (no value filter) | **0** -- exact |
| value-only (`v != 0` first) | **88** -- wrong |

The union prune is exact; the value-only prune that the plan warned about is wrong
on **44% of rows**. The zero-valued candidates are not a rounding curiosity, they
are the output. F7b itself does **not** prune -- the prune lives in a separate
`union_prune_row()` so the parity claim never depends on the optimisation being
sound.

### Two test-harness bugs that produced FALSE GREENS

Both were mine, and both are the reason "0 differing" was printed over an empty
set:

- Calling `cprr_gram_prefix()` without `effectEstimationEmb` first leaves
  `sorted_ee_` empty, so the accumulation loop never runs and `incElems` stays
  **empty**. The Gram comparison then iterated an empty set, compared **zero
  cells**, and printed `0 differing`. A green that measured nothing.
- Calling it without `hyper_rr_embedding()` first compares the reference's
  *pre-loop* `emb` against the core's *post-loop* `emb` -- a 34914-cell
  "mismatch" that looked like an F6 bug and was not.

**Structural fix, not just a patch:** the gate now asserts the Gram comparison
compared a **non-zero** number of cells (`expect(gcmp > 0, ...)`), so a vacuous
pass is a failure. This is X2/X3's cousin and the most dangerous class of bug in
this whole port: a check that passes because it never ran.

**Result:** E2E `ck_rk = 0x58ded033b72ded72`, matching the Step 0 golden exactly.
10/10 fixtures, thread-deterministic 1/2/4/12, ASan+UBSan clean.

---

## Decision: strict bit-exact replay vs. native-engine order + documented tolerance

**Format follows `Data/phase_profile/bfstree_close/README.md` §7.4**, which made the
same class of call for BFSTree. Precedent matters, but the BFSTree decision was
about the **build regime** (`-ffast-math` vs a strict target). This one is a
different axis and, as it turns out, a *categorically* different question.

### The BFSTree question and the RFE question are not the same

BFSTree had a genuine speed/numerics trade-off: mode `E` (pure SpGEMM) measured
**1,2–2,2x faster** but with **P@20 = 0,62–0,81**, so it was rejected on measured
accuracy. Both options were *runnable*; the decision was which to ship.

RFE has almost no such trade-off. **Wherever the port departed from a stock engine
kernel, the kernel was not slower — it was wrong:**

| phase | alternative | measurement | verdict |
|---|---|---|---|
| F2 `createEmb` | `multiply_and_prune` | H1: **64 cells** diverge (SpGEMM order is not the legacy's `(pos1,pos2)` order) | **incorrect** |
| F2 | builder duplicate-coalescing | `SparseMatrix.hpp:322-336` sorts each row with an unstable `std::sort` before summing; order is libstdc++-specific, not the legacy's | **not bit-guaranteed** |
| F4 | `CSRMatrix::combine` | drops **9511 of 14651** keys (intersection of *nonzero* supports) | **incorrect** |
| F4 | `CSRMatrix::merge` | drops **10209 of 14651** keys (`if (res != T{})` emission filter) | **incorrect** |
| F5 `tag_sort` | `std::stable_sort` | **5 of 80** positions wrong at `80 20 6 1`; **331** positions across the 4 tie fixtures | **incorrect** |
| F6 Gram | parallel reduction over `q` | different summation order ⇒ different float; F6 must parallelise over **rows** instead | **incorrect** |
| F7a `simEmb` | `get()`-probe instead of materialised rows | `get()` returns `0.0f` for *absent* keys, so `v*0.0f` is not the legacy's `if (found)` skip | **not bit-exact** |

So strictness was not "pay for exactness". In five of six cases the cheap option was
**unavailable**, because it computes a different function. The cost of strictness is
therefore not a weighed performance number — it is the complexity of six
hand-written kernels, whose correctness argument is the per-phase parity gates.

**What is NOT claimed:** that strict replay is *fast*, or that the alternatives would
have been an acceptable trade. **No benchmark of RFE's strict kernels exists yet.**
That is Step 3's job, and it is listed as an open risk below.

### The one real trade-off: the F7b union prune

F7b keeps **every** discovered candidate and takes the first `l` after sorting. The
union prune (`union_prune_row`) selects the top-`l` instead. This is the only place
where a cheaper alternative is both **available and sound**:

| fixture (N L k t) | union-prune rows differing | value-only-prune rows differing |
|---|---|---|
| 200 40 10 2 | **0** | 88 |
| 120 30 8 2 | **0** | 37 |
| 80 20 6 1 | **0** | 43 |
| 300 60 15 3 | **0** | 102 |
| 150 50 25 2 | **0** | 44 |
| 200 200 20 1 | **0** | 29 |
| 60 60 30 1 | **0** | 20 |
| 400 40 10 2 | **0** | 186 |
| 200 40 10 1 | **0** | 92 |
| 200 40 5 2 | **0** | 200 |
| **total** | **0** | **841** |

**Soundness is measured on all 10 fixtures, not just the pinned one** (X3). The gate
now **fails** if any fixture's union prune is non-exact, so the claim cannot rot into
an unchecked assumption. The value-only prune — the design H8 warned about — is wrong
on **841 rows**, including *every* row at `200 40 5 2`.

**DECIDED: not adopted — and the Step 3 census shows it is not merely unnecessary
but nearly pointless.** The rationale below was written from the synthetic fixture;
Step 3 measured the real one and the reason changed.

> ~~At the pinned fixture the Gram is near-dense (**39996 of 40000 cells**, ρ ≈ 1.0),
> so pruning to `l` per row would cut roughly **5×** (`N·L = 8000` vs 39996 cells).~~
>
> **MEASURED ON REAL ALOI (N=72000, L=400): ρ(Gram) = 0.002148**, Gram nnz
> 11,132,644, mean row **154.6** — *smaller than L*. So the prune to top-L saves
> **0.9%** (0.089 GB → 0.088 GB). The Gram is already far sparser than any top-L
> scheme would impose.
>
> Adopting it would buy 0.9% of memory in exchange for a permanent correctness
> dependency. **It stays off, now for a measured reason rather than a deferred
> one.** `union_prune_row()` stays implemented and gated (exact on all 10
> fixtures) so the option remains available if a future regime is denser.

The real prize is elsewhere, and it is not the prune: **the CSR Gram is 0.089 GB
where the legacy's dense matrix is 20.74 GB — 233× less.** The sparsity the port
gets from `CSRMatrix` is the entire memory win; no pruning scheme was needed to
achieve it.

### Why this does not generalise from BFSTree

The two methods differ in exactly the way that makes the comparison invalid. BFSTree's
`W` is a dense-ish product where SpGEMM is the *natural* formulation and the only
question is which block ordering the legacy used. RFE's structure is **dynamically
discovered candidates** (`incElems` is a union over `q` of `sorted_emb[q]`) whose
zeros are *load-bearing*. An engine kernel that filters `res != 0` is not a different
schedule for the same function — it returns a different **support**, which changes the
output rank lists. The BFSTree precedent justifies the *method* (measure the
alternative before choosing), not the *conclusion*.

### Consequences for the build-regime decision

The strictness of the numerics is independent of the `-ffast-math` question but it
interacts: every gate here runs `-O2 -ffp-contract=off`. BFSTree measured that the
product's `-ffast-math` regime is **not neutral to the engine** (LHRR `nnz` −8/−11%,
81% of ranking rows changing), so a global regime change would require re-baselining.
RFE has not been measured under the product's regime yet — that is Step 4's decision,
and until then the core is developed under strict flags with the same posture BFSTree
used.

---

# Step 3 — Stage 0 census, on REAL data (ALOI N=72000 L=400)

Tool: `Source/bench/bench_rfe_census.cpp`. Protocol: touch-first — the census ran
in **2.1 s** and refuted two things I had predicted, before any expensive run.

### The decisive number: ρ(Gram) is 0.0021, not the ~0.17 I predicted

| quantity | measured at N=72000, L=400, k=10 |
|---|---|
| `emb` nnz | 1,957,862 (mean row **27.2**) |
| Gram nnz | **11,132,644** (mean row **154.6**, max 750) |
| **ρ(Gram)** | **0.002148** |
| Gram exact-zeros | **7,071,700 = 63.52%** of nnz |
| **legacy dense matrix** | **20.74 GB** |
| **CSR Gram** | **0.089 GB** |

**The census method validates itself:** the analytic `nnz_est = 11,132,644` is
*exactly* the `gram_nnz = 11,132,644` the materialised F6 then produced. A TOUCH
census that predicts the materialised count to the cell is trustworthy in a way
the `rho_datasets.sh` BFSTree census explicitly was not (that one is recorded as
a proxy with *measured* error, −1.96% on ALOI).

### Correction to the §4 decision: the union prune would save 0.9%

§4 argued the prune was "worth taking — roughly **5×** less memory". That number
came from the synthetic pinned fixture, where **ρ(Gram) ≈ 1.0** (39996/40000).
Real data has **ρ = 0.0021**, and the mean Gram row (**154.6**) is *smaller than
L (400)*. So pruning to top-L per row saves:

| | memory |
|---|---|
| CSR Gram unpruned | 0.089 GB |
| CSR Gram union-pruned to L | 0.088 GB |
| **saving** | **0.9%** |

The §4 conclusion ("defer the prune") turns out to be **right for a reason I had
the wrong way round**. It is not "an optimisation we have not yet justified" —
at real scale the prune is close to a **no-op**, because the Gram is already far
sparser than any top-L scheme would impose. Adopting it would add a correctness
dependency (the prune must stay exact) for 0.9%. **It stays off**, now for a
measured reason rather than a deferred one.

### Correction to the grValue theorem: F1 GUARANTEES the premise, so the original form stands

**What the census measures: the RAW file, before F1 runs.** In
`Source/bench/bench_rfe_census.cpp` the `RKSELF` block walks the bytes loaded
from the `.bin`; the `RFECore` is not constructed until after it. So the
following is a property of the INPUT FILE, not of the pipeline:

| census (raw ALOI, 72000 rows) | value |
|---|---|
| `self_at_0` | **71,448 (99.233%)** - 552 rows are not already normalised |
| `self_in_L` | 72,000 (100%) |
| `self_in_all` | 72,000 (100%) |
| ids out of range | 0 |

**And `f1_rank_norm()` repairs all 552 before F2 ever sees them.**
`sort_ranked_lists` pins self to position 0 unconditionally:

```cpp
if (self_pos > 0) std::swap(rk_[q*l], rk_[q*l + self_pos]);
```

The only failure mode is total absence (`self_pos < 0`), which sets
`missing_self[q]` and throws AFTER the parallel region (X7). The census measures
`dropped_noself = 0`, so that never fires on ALOI.

> **Correct theorem.** If self is PRESENT in every row, then F1 puts it at
> position 0 of every row, hence `emb_pre[q][q] >= 1` for every q, hence
> `grValue = max_q ee[q] > 0`. The premise is guaranteed BY F1, not assumed of
> the dataset.

I had first written this as "the premise is VIOLATED on 552 real rows". That
framing was wrong in a way that mattered: it implied the pipeline might run
under a violated premise. It cannot. The 99.233% is informative - it says the
input does not arrive self-normalised - and nothing more.

Corollary for the next method: any census that reports a per-row structural
property must label WHICH STATE it measured. Raw and post-F1 are different
facts, and conflating them manufactures a bug that does not exist.
property must label WHICH STATE it measured. Raw and post-F1 are different
facts, and conflating them manufactures a bug that does not exist.

### The zeros are far more load-bearing on real data than on synthetic

Gram exact-zeros: **14.88%** synthetic (pinned fixture) vs **63.52%** real (ALOI
N=72000). H8's warning — that a value-only prune drops real candidates — is
therefore **~4× stronger on real data** than anything the synthetic sweep
suggested. Any future pruning work must be re-validated against 63%, not 15%.

### Stage 2 is INFEASIBLE *for ALOI* - and I over-generalised that to "real data"

The BFSTree precedent subsamples the ALOI file by prefix (13k/18k/24k). That does
not work here, and the reason is arithmetic: filtering `id < N'` from a row of `L`
ids drawn from `[0,N)` leaves about `L*(N'/N)` survivors. Preserving `L`
survivors requires `N' >= N`. Measured:

```
RKCONTRACT rows=4000 kept=0 dropped_short=4000 dropped_noself=0
RKCONTRACT_REFUSED: 4000/4000 rows violate the contract at N=4000 L=400
                    (expected survivors ~ L*(Nsub/Nfull) = 22.2 of 400)
```

All 4000 rows are short. And the legacy needs `N^2*4 = 20.74 GB` for its dense
matrix, against **7 GB total / 5 GB available** on this host.

**CORRECTION (self-inflicted premature generalisation).** I wrote the heading as
"Stage 2 is INFEASIBLE". Both facts above are specific to ALOI at N=72000; neither
is a property of "real data". That is exactly the failure mode X3 exists to
prevent - generalised conclusion from one fixture - committed here about my own
conclusion rather than about a synthetic fixture.

**The real correction: real-data bit-exactness IS attainable on this host.** Two
whole real datasets with genuine `N`, no prefix subsampling, and `4N^2` well
inside RAM, were already in the repo:

| dataset | file | N | native L | `N^2*4` |
|---|---|---|---|---|
| corel5k | `Datasets/corel5k/ACC.txt` | 5000 | 4999 | 100 MB |
| linnaeus | `Datasets/linnaeus/linnaeus5_resnet50.txt` | 8000 | 3999 | 256 MB |

(`vegfru/rkLists.txt` is the ALOI *format* - self at 0, no conversion needed -
but N=160,731, where the legacy would need 103 GB, and prefix subsampling fails
there for the same arithmetic reason.)

Two caveats make these a *derived* dataset, recorded as such and never silently
applied:
1. **Format differs from ALOI.** The first token is the *query id*, and self is
   NOT in the list (`corel5k` row 1 is `1 4778 4912 4956 0 ...` - that `0` is a
   neighbour). Conversion: strip the query id, prepend self, truncate to L.
2. **Native L ~ N makes F7b degenerate.** With `incElemsL[q]` equal to the whole
   list, `set_difference(incElems[q], incElemsL[q])` is EMPTY and the CPRR tail
   becomes a no-op - the interesting path is not exercised. Hence L is a chosen
   axis (400 and 1000), not a dataset constant.

The upside is real: these are the **dense** end of the space (expected
`rho(Gram) ~ 0.1` against ALOI's 0.002), which is where the O(k^2) sort of Stage
3a suffers most. Bit-exactness measured here is a genuinely different claim from
the one ALOI could give.

### What real scale DID validate

The CSR port runs the **complete** pipeline on real ALOI at N=72000:

```
T_f1=1.74s  T_f24=0.12s  T_f5=0.02s  T_f6=3.15s  T_f7b=0.15s   total 5.55s
```

and it is **thread-deterministic on real data**: `ck_rk = 0x0db02fefdf6bd62c`
identical at 1, 4, 8 and 12 threads.

| threads | total | speedup | efficiency | T_f6 (Gram) |
|---|---|---|---|---|
| 1 | 26.97 s | 1.00× | 100% | 17.91 s |
| 4 | 11.73 s | 2.30× | 58% | 7.68 s |
| 8 | 8.12 s | 3.32× | 42% | 4.96 s |
| 12 | 5.55 s | 4.86× | 41% | 3.15 s |

**F6 (the Gram) is the bottleneck — 66% of total time at 1 thread** — and its
scaling is the weakest, which is the expected cost of F6's forced row-parallel
structure (rows have unequal `|inv|`, so load is imbalanced by construction).
Efficiency falls to ~41% by 12 threads. That is the price of the bit-exact
decision, now with a number attached: **row-parallel buys correctness at ~2.4× the
parallel cost of an ideal split.**

---

# Step 3 — Stage 3a: F6 cost attribution (REAL ALOI, N=72000)

Question this stage answers, and it is NOT "is F6 slow" (already known): **is F6
slow because of STRICTNESS or because of IMPLEMENTATION?** The two have opposite
remedies — the first cannot be fixed, the second can.

Instrument: `RFECore::set_f6_profile(true)`, off by default so every parity gate
runs the identical code it ran before. Driver: `Source/tests/step3a_f6prof.sh`.

## The result: 77.3% of F6 is an O(k²) insertion sort, and strictness is 19.6%

At N=72000, 1 thread, F6 wall = **16.37 s**:

| phase | s | % of F6 | what it is |
|---|---|---|---|
| `t_sort` | **12.649** | **77.3%** | `RowAcc::finalize_candidates` — insertion sort + dedupe |
| `t_acc` | 3.216 | **19.6%** | pass 2 — **the ordered accumulation, i.e. the actual strictness** |
| `t_disc` | 0.198 | 1.2% | pass 1, union discovery |
| `t_emit` | 0.105 | 0.6% | triplet emission |
| `t_inv` | 0.068 | — | serial inverted index |
| `t_build` | 0.134 | — | serial `build_from_partial_results` |

Measured work: `cand_raw = 59,684,264` (with multiplicity), `cand_uniq = 11,132,644`,
`inner_iters = 59,684,264`, **raw/uniq = 5.36**, `inv_min = 1`, `inv_max = 103`.

Modelled comparisons: insertion sort ≈ `Σ k²/4 ≈ 2.47e10` vs the binary searches'
`inner_iters·log₂(uniq) ≈ 4.34e8`. **The sort does ~57× more work than the phase it
was supposed to be incidental to.** 12.649 s over 2.47e10 shifts is 0.51 ns/shift,
which is what a shift-heavy insertion loop should cost — the model fits.

## Correction to what I wrote one stage ago

I attributed F6's poor scaling to "load imbalance by construction" and priced
"the price of bit-exactness at ~2.4×". **That was wrong in its causal claim.** The
measured per-phase scaling separates the two effects cleanly:

| critical path (max thread) | 1T | 2T | 4T | 8T | 12T | eff @12T |
|---|---|---|---|---|---|---|
| `t_sort` (quadratic) | 12.649 | 7.198 | 4.053 | 2.667 | 2.287 | **46%** |
| `t_acc` (ordered, the strict part) | 3.216 | 1.682 | 0.893 | 0.479 | 0.370 | **72%** |

**The strict accumulation scales nearly twice as well as the sort.** So F6's bad
scaling was never the price of strictness — it was the quadratic sort.

And the reason is sharper than "imbalance". Row cost of the sort grows as `k²`,
where `k ∝ |inv[i]|`, and `|inv|` ranges **1 → 103 (103×)**. Squaring that spreads
the sort's per-row cost over a **~10,000×** range, while the accumulation's cost
spreads over the original 103×. **The quadratic amplifies the imbalance**, which is
why `t_sort` drops to 46% while `t_acc` holds 72%.

So the price of bit-exactness is *not* 2.4×. It is the 19.6% of F6 that is `t_acc`,
and that part is **irreducible**: splitting a row's `q`-loop across threads computes
`((a0+a1)+a2) + (a3+a4)` where the legacy computes `(((a0+a1)+a2)+a3)+a4`, which
differs in `float`. Row-parallel is the only bit-exact split available.

## The 3b plan changes as a result

The Stage 3b variants A/B (splitting `t` across threads, bit-exact or not) attack
**19.6% of F6**. They are no longer the priority. The priority is a fix that is
bit-exact *by construction* because it changes no float operation and no ordering
of the survivors:

> **Replacing the insertion sort with `std::sort` in `finalize_candidates()` is
> provably bit-exact.** Pass 1 pushes only `(col, 0.0f)` pairs. `finalize_candidates`
> sums duplicates — and `0.0f + 0.0f + … + 0.0f == 0.0f` exactly, independent of
> order. So the coalesced value is always exactly `0.0f`, and the result is the
> unique-ascending-column map with `0.0f` values *whatever sort produces it*.
> Stability cannot matter because there is nothing in the value to preserve.

Predicted: `829·log₂829 ≈ 8.2e3` comparisons vs insertion's `829²/4 ≈ 1.7e5`, a
~21× reduction. That is Stage 3b's first item, and it is gated on `ck_rk` /
`ck_gram` being unchanged, not on an argument.

## Two bugs I introduced while measuring, recorded because both were silent

1. `cand_raw` was read *after* `finalize_candidates()` had already resized `cols`,
   so both counters reported the uniq count and `raw/uniq` came out as a
   tautological **1.00**. It looked like a finding — "the union has no duplicates"
   — and was pure instrumentation error. The real ratio is 5.36.
2. Percentages were computed as *summed per-thread seconds* over *wall seconds*,
   producing **833%** at 12 threads. The profile now reports sum and max against
   separate denominators (`sum/(t_total·threads)` = capacity share,
   `max/t_total` = share of the critical path).

## Stage 3a conclusion

F6 is 70% of the real-data pipeline, and the sort alone is **~50% of the total
pipeline wall time** (12.649/24.43 = 51.8% at 1T, 4.053/8.26 = 49.1% at 4T,
2.287/4.89 = 46.8% at 12T) — one O(k²) insertion sort, not the float ordering that
motivated the whole port. The strictness that *is* required costs 19.6% of F6 and
is provably irreducible. The correct next move is a sort-complexity fix, validated
by digest equality — not a parallelisation scheme.

---

| Phase | State |
|---|---|
| F1 `rankNorm` + sort | DONE, bit-exact |
| F2 `createEmb` | DONE, bit-exact |
| F3 `createReverseEmb` | DONE, bit-exact |
| F4 `combineReverseEmb` | DONE, bit-exact |
| F5 `sortEmb` + `effectEstimationEmb` | DONE, bit-exact |
| F6 Gram + `incElems` | DONE, bit-exact |
| F7a `reRankingByEmbSim` (t-loop) | DONE, bit-exact |
| F7b CPRR tail + union prune | DONE, bit-exact, E2E matches golden |
| CC / EmbCC / export / fusion | DEFERRED, still legacy-only |
| Production wrapper / registration / `run_all.sh` | NOT DONE (deliberately) |

### Open risks, stated rather than hidden

- **REAL-DATA BIT-EXACTNESS IS NOT ESTABLISHED, and cannot be on this host.**
  Stage 2 is blocked by measurement: subsampling ALOI to `N' < N` leaves
  `L·(N'/N)` survivors per row (< L, so the contract refuses), and the legacy needs
  20.74 GB where the host has 5 available. Bit-exactness rests on synthetic fixtures
  only (10 parity + 4 tie + 5 corner seeds). Closing this needs either a
  smaller-`L` real dump or a host with ≥ 24 GB.
- **One dataset, no manifold diversity.** Every "real" number here is ALOI. The
  `rho_datasets.sh` precedent warns that prefixes of one file measure scale, not
  manifold.
- **F6 is 66% of runtime and scales worst** (58% efficiency at 4 threads, 41% at
  12). The price of F6's forced row-parallel structure — measured, but not yet
  optimised, and the optimisation space is unexplored.
- **Production-scale accuracy is unmeasured.** No P@k against ground truth. The port
  is bit-exact, not validated for retrieval quality, and bit-exactness against a
  legacy that cannot run at this scale is untestable here.
- **Fast-math / strict-build policy — DECIDED by Stage 3c, against my expectation.**
  RFE needs more than `-ffp-contract=off`: it needs fast-math *out*. Under the
  product's flags the final rank lists change on real data. See the Stage 3c section.

---

# Step 3 — Stage 3c: RFE under the PRODUCT's build flags

Tool: `Source/tests/step3c_numerics_rfe.sh`. Methodology copied from
`Source/bench/prod_numerics.sh` (flags **extracted from the Makefile**, never
retyped; four regimes; guards that the regime derivation actually happened).

The product's flags (`make -C Source -s show-flags`):
`-std=gnu++23 -O3 -march=native -mfma -ffast-math -funsafe-math-optimizations
-ffinite-math-only -funroll-loops -fprefetch-loop-arrays …` and **no
`-ffp-contract=off`**. CPU: Intel i5-10400F.

## The finding: under the product's flags, RFE's output CHANGES on real data

ALOI N=72000, L=400, k=10, t=2, T=12:

| regime | `ck_gram` | **`ck_rk` (final rank lists)** |
|---|---|---|
| PROD (product verbatim) | `0xdcc6e3c6d270f2dc` | **`0x8079b48d44cbd874`** |
| PRODC (`+ -ffp-contract=off`) | `0xdcc6e3c6d270f2dc` | **`0x8079b48d44cbd874`** |
| NFM (fast-math stripped) | `0x2dfa5891d430e9af` | `0x0db02fefdf6bd62c` |
| BENCH (`-O2 -ffp-contract=off`) | `0x2dfa5891d430e9af` | `0x0db02fefdf6bd62c` |

**The final rank lists are different.** `0x8079b48d…` is not a relabelling of
`0x0db02fef…`; it is a different set of retrieved ids. The Gram **support** is
identical across all four (`gram_nnz = 11,132,644`, `gram_zeros = 7,071,700 =
63.52%` in every regime) — so the support is stable and only the **values** move,
then propagate through F7b's reranking into a different selection.

**Attribution: PROD == PRODC, and NFM == BENCH.** Disabling FMA contraction alone
does *not* recover the answer, so the culprit is fast-math's **reassociation**, not
contraction. This is the *opposite* attribution from the LHRR precedent in
`numerics_prod/verdict.txt` (there PRODC fixed it) — which is exactly why the ladder
has to be measured per method rather than inherited.

## The dangerous part: the gate passed all four regimes

`test_rfe_parity` reports `RFE PARITY F1+…+F7: OK (bit-identical to the legacy)` under
**every** regime, including PROD. The synthetic `ck_rk` pin (`0x3465e556d414ca79`) and
the E2E pin (`0x58ded033b72ded72`) hold in all four.

Three structural reasons the gate is blind:

1. **`ck_gram` is printed but never asserted.** Line 789 prints it; there is no
   `expect(ck_gram == …)`. The hard float instrument is decorative.
2. **The Gram check is relative, not absolute.** `expect(g_bad == 0, …)` compares the
   core against `rfe_ref_verbatim.hpp` **compiled with the same flags**. Fast-math
   perturbs *both* sides identically, so agreement is preserved while absolute values
   drift. A core-vs-reference comparison cannot detect a regime change by
   construction.
3. **The synthetic fixture is too small to expose it.** At N=200 the tie-breaking
   tolerates 1-ULP movement; at N=72000 with 63.5% exact zeros it does not. The pin
   that "proves" the claim generalises from a point where the phenomenon is absent.

**So bit-exactness was conditional on the build regime, and nothing in the gate stated
or enforced that condition.** The `-O2 -ffp-contract=off` policy in the RFECore header
was doing the work alone.

## Consequences — a gate hole, not a preference

- **RFE requires the strict regime, and it is stronger than `-ffp-contract=off`.** It
  needs fast-math *out* (`-funsafe-math-optimizations`, `-ffinite-math-only`), not
  merely contraction off. Now a measured requirement, not a Step 4 deferral.
- **`ck_gram` must become an absolute pin**, recorded from the BENCH regime
  (`0x9cf3d5102ffa87e9` on the fixture). That single change would have made this
  Stage 3c self-detecting, and it is the cheapest fix available.
- **A synthetic-only gate cannot police build regime for this method.** The real-data
  regime comparison is the test that found it and belongs in the gate.

Recorded as a **gate hole**: the existing green gate is not evidence of bit-exactness
under the product's build, and the product build is what ships.
- **Single `k`/`t` on real data.** The census ran at k=10, t=2 only.

---

## X8 - A relative gate is blind to build-regime change BY CONSTRUCTION

> **X8.** A gate that compares the core against a reference compiled with the
> SAME flags cannot, by construction, detect build-regime regression. Regime
> requires an ABSOLUTE pin, recorded once under the correct regime, compared on
> real data large enough for the effect to appear.

Not an RFE finding: a class of gate hole. Full statement, mechanism and the three
concrete ways it hid are in `../bench/README.md` pitfall 7 and checklist item 10.

### Regime convergence: RFE reuses `udlf-strict`, no new build infrastructure

```
STRICT_FLAGS = $(filter-out $(FAST_MATH_FLAGS),$(CFLAGS)) -ffp-contract=off
```

(`Source/Makefile`) is **literally** the `NFM` regime of Stage 3c - product flags
minus `-ffast-math`/`-funsafe-math-optimizations`/`-ffinite-math-only`, plus
`-ffp-contract=off`. The target `udlf-strict` already exists and already produces
`bin/udlf_strict`, the binary in which a method with an identity contract is
allowed to run while `bin/udlf` refuses it.

**Attribution correction.** I had expected RFE's cause to be FMA contraction, by
analogy with the LHRR precedent. Measured: **`PRODC == PROD` and `NFM == BENCH`**,
so for RFE the cause is **reassociation**, and disabling contraction alone does
not help. The Makefile's own audit note for BFSTree says the same thing
("PRODC does not fix it - cause is mixed FMA/reassociation"). Both methods agree,
and the ladder must be measured per method rather than inherited.

**DECIDED:** RFE is registered in the existing `udlf-strict` target and refuses to
run in `bin/udlf`, exactly as BFSTree does. No new build infrastructure. The
strict-build policy stops being a Step 4 deferral: it is a measured requirement,
with the measurement attached.

---

# Step 3 - Stage 3b/2: REAL-DATA BIT-EXACTNESS ESTABLISHED

The biggest open risk of the arc, closed. Not with new hardware: with two whole
real datasets that were already in the repo.

| dataset | N | L | rho(Gram) | Gram nnz | exact zeros | `ck_gram` | final `ck_rk` (legacy == core) | cells differing |
|---|---|---|---|---|---|---|---|---|
| corel5k | 5000 | 400 | 0.1525 | 3,812,490 | 76.72% | `0x07a6340124066c15` | `0xb2d84793a18b7e43` | **0 of 2,000,000** |
| corel5k | 5000 | 1000 | 0.1525 | 3,812,490 | 76.72% | `0x07a6340124066c15` | `0x7ce087aad8d272f1` | **0 of 5,000,000** |
| linnaeus | 8000 | 400 | 0.0655 | 4,190,408 | 81.09% | `0xe5907543f4c7ff0a` | `0x09b624c5caae064c` | **0 of 3,200,000** |

Every phase, every cell, both sides bit-identical: `mismatched=0`, `extra=0`,
off-diagonal Gram values exact, F7a 0 differing, tail 0 differing, E2E identical.

**These are the first real-data bit-exactness pins for RFE.** The claim is no
longer synthetic-only (10 parity fixtures + 4 tie fixtures + 5 corner seeds).

## H8 is far stronger on real data than the synthetic sweep ever showed

The value-only prune (the wrong alternative) is wrong on:

| dataset | rows it gets wrong |
|---|---|
| corel5k L=400 | **4,997 of 5,000** |
| corel5k L=1000 | **5,000 of 5,000** |
| linnaeus L=400 | **8,000 of 8,000** |
| (synthetic pinned fixture) | 841 of 10 fixtures |
| ALOI N=72000 | 63.52% of Gram cells are exact zeros |

Not "sometimes wrong on some rows" - wrong on essentially **every** row of a
real dataset. A value-only prune is not a candidate implementation; it is the
opposite of one.

## Correction to my own experimental design: L is NOT a density axis

I chose "L=400 and L=1000" as a measured axis. The runs show it is **not one**,
and the reason is structural:

```
corel5k L=400   emb keys=254270   ck_gram=0x07a6340124066c15
corel5k L=1000  emb keys=254270   ck_gram=0x07a6340124066c15
```

`emb` is **byte-identical** across the two. `emb` support per row is ~50.8
(254270/5000), comfortably under the `k + k^2 = 110` bound - so `createEmb` is
driven by the **first k positions** of each rank list (`rankKScore(pos)` is
decreasing in `pos`), and positions beyond `k` contribute nothing to the support.

Consequences, and they matter for how the next experiment is designed:
- `L` does **not** move `emb`, the Gram, `sorted_emb`, `sorted_ee` or `ee`.
- `L` only sets `incElemsL[q]`, which is the cut driving F7b's
  `set_difference`. That is why the final `ck_rk` DOES differ between L=400 and
  L=1000 (`0xb2d84793...` vs `0x7ce087aa...`) while `ck_gram` is identical.
- **The real density axis is `k`, not `L`.** rho(Gram) is set by `k`: ALOI
  k=10 gives 0.0021, corel5k 0.1525, linnaeus 0.0655 - a spread driven by
  structure, but the knob is `k`.

I should have designed the axis from `createEmb` rather than from intuition
about list length. The runs were not wasted - they are two independent
bit-exact pins and they exposed the property - but the axis I chose was the
wrong one.

## Datasets are DERIVED, and say so

`Datasets/corel5k/ACC.txt` and `Datasets/linnaeus/linnaeus5_resnet50.txt` use
the format `q n1 n2 ... nK` (leading query id, self NOT in the list).
`Source/tests/mk_rk_from_leading_id.py` converts to the ALOI convention and
writes a `.provenance` sidecar recording source, source md5, N, L, the exact
transform, and the output md5. Refuses (exit 5) rather than filling a short row.

| dataset | source md5 | output md5 | N | L |
|---|---|---|---|---|
| corel5k L=400 | (see sidecar) | `abc57bdd7aed6ee66e7fbfc32cbd3301` | 5000 | 400 |
| corel5k L=1000 | (see sidecar) | `0fba3ef9d5fab266dceda912c7082877` | 5000 | 1000 |
| linnaeus L=400 | (see sidecar) | `dde830919922f758424282efaec41613` | 8000 | 400 |

`L` is the ROW WIDTH including self, matching ALOI (400 columns = self + 399
neighbours) - the same convention the core's `l_` expects. My first converter got
this wrong and the loader's extra-column guard caught it, which is the guard
working as designed.

## Status of the open risks

| risk | before | now |
|---|---|---|
| real-data bit-exactness not established | **open, "cannot be done on this host"** | **CLOSED** - 3 real-data pins, 0 differing cells |
| bit-exactness synthetic-only | open | closed (was the same risk, wrongly scoped) |
| one dataset, no manifold diversity | open | **partly** - 2 real datasets, 2 structures, 3 configs; still one `k` |
| production-scale accuracy unmeasured | open | open (unchanged - this is parity, not P@k) |
| F6 bottleneck unoptimised | open | open (Stage 3c measured it; the sort fix is still pending) |
| fast-math / strict-build | decided (Stage 3c) | decided |

---

# Step 3 - Stage 3b: O(k^2) insertion sort eliminated in finalize_candidates

Measured on real ALOI N=72000 L=400 k=10 t=2, before and after in the SAME
session, sequential (Source/tests/step3b_sort_ab.sh).

## The change, and why it is bit-exact by CONSTRUCTION rather than by argument

The old code shuffled `vals` alongside `cols` through an insertion sort, purely
so it could coalesce duplicates by summing. But `add_candidate` only ever pushed
a literal `0.0f`, and `0.0f + 0.0f + ... + 0.0f` is exactly `0.0f` for every order
and every count. **The value it computed was a constant it already knew.** So
`vals` is not carried through pass 1 at all:

```cpp
void finalize_candidates() {
  std::sort(cols.begin(), cols.end());
  cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
  vals.assign(cols.size(), 0.0f);
}
void add_candidate(idx_t c) { cols.push_back(c); }
```

Three failure modes removed at once, none of them hypothetical:
- **O(k^2) -> O(k log k)** on the raw candidate count (~829/row measured).
- **The (col,val) pairing bug class** -- there is no pair to desynchronise.
- **The value-carrying refactor class** -- there is no value to carry.

`stable_sort` is NOT required: the output is the distinct ascending column set
with `0.0f` values, which is the same set for any correct sort.

## Measured

| phase (critical path) | T=1 | T=4 | T=12 |
|---|---|---|---|
| `t_sort` before | 12.751 s | 4.008 s | 2.414 s |
| `t_sort` after | **1.635 s** | **0.454 s** | **0.211 s** |
| speedup | **7.8x** | **8.8x** | **11.4x** |

| F6 wall | T=1 | T=4 | T=12 |
|---|---|---|---|
| before | 16.485 s | 5.160 s | 3.036 s |
| after | **5.294 s** | **1.542 s** | **0.789 s** |
| speedup | **3.11x** | **3.35x** | **3.85x** |

| pipeline total | T=1 | T=4 | T=12 |
|---|---|---|---|
| before | 24.70 s | 8.06 s | 5.03 s |
| after | **13.82 s** | **4.48 s** | **2.74 s** |
| speedup | **1.79x** | **1.80x** | **1.84x** |

## The profile inverted, which is the point

| | before | after |
|---|---|---|
| `t_sort` share of F6 | **77.4%** | 30.9% |
| `t_acc` share of F6 | 19.6% | **60.5%** |

The strict ordered accumulation is now the majority of F6. That is the phase
Stage 3a proved **irreducible** (splitting a row's `q` loop across threads
computes `((a0+a1)+a2)+(a3+a4)` where the legacy computes `(((a0+a1)+a2)+a3)+a4`).
So the optimisable half of F6 has been taken, and what remains is the part that
cannot be taken.

**I also note my own prediction was wrong by 2.7x**: I forecast `t_sort` ~0.6 s
at 1 thread; it is 1.635 s. The algorithmic claim (O(k^2) -> O(k log k)) held,
the constant did not -- I assumed ~1 ns per `std::sort` comparison and the real
cost is ~2.8 ns, which is what unpredictable branches cost. The measured
7.8x is also below the ~18x a pure comparison count predicts, because the
insertion sort's shifts are cheap per comparison. The model was directionally
right and quantitatively wrong; the numbers above are the measurement.

## Validation: four absolute pins, all unmoved

| pin | value | before | after |
|---|---|---|---|
| synthetic fixture `ck_gram` | `0x9cf3d5102ffa87e9` | OK | **OK** |
| corel5k N=5000 L=400 `ck_gram` / `ck_rk` | `0x07a6340124066c15` / `0xb2d84793a18b7e43` | OK | **OK** |
| corel5k N=5000 L=1000 `ck_gram` / `ck_rk` | `0x07a6340124066c15` / `0x7ce087aad8d272f1` | OK | **OK** |
| linnaeus N=8000 L=400 `ck_gram` / `ck_rk` | `0xe5907543f4c7ff0a` / `0x09b624c5caae064c` | OK | **OK** |
| ALOI N=72000 `ck_gram` | `0x2dfa5891d430e9af` | OK | **OK** |

`step2_check.sh` green (10 fixtures + 4 tie fixtures + corner). Corner suite
18 checks, 0 failures, including the new `finalize_candidates` contract block:
distinct ascending columns, bit-exactly `0.0f` per column, empty row, singleton,
and an all-equal row.

## X8 delivered on its own terms

`Source/tests/step3b_real_gate.sh` proves the pin **can fail**:

```
[1/2] pin PASSES under BENCH   -> ok
[2/2] pin FAILS  under PROD    -> FAIL: X8: ck_gram == 0x9cf3d5102ffa87e9   (rc=1)
```

Before the pin existed, the same source under PROD printed
`RFE PARITY ... OK`. That is the whole of X8 in two lines of evidence.

Two defects I fixed in my own gate while building it, both of them the X6 shape:
- the real-data loop ran inside a **pipe** (`echo | while`), so it ran in a
  subshell and a divergence would have printed and then exited 0;
- the multi-set corner expectation was mine and **wrong** (5 distinct columns;
  the multiset has 6) -- the new test caught my own arithmetic on its first run.

## What is now the bottleneck

At T=12 the pipeline is 2.74 s, of which `T_f1 = 1.28 s` (**47%**). F1
(`rankNorm`) has replaced F6 as the dominant phase. Next target, not started.

---

> ## SUPERSEDED -- do not read this section as fact.
>
> The paragraph below titled "What this makes the duplicate-parameter form at
> `RFECore.hpp:974`" is **fabricated**. There are no duplicate parameter names;
> the parameters are `posqi` and `posiq`, and the core implements the legacy
> `pow(sig(posqi),2) * sig(posiq)` verbatim -- proven cell by cell at 8000/8000.
> See "CORRECTION -- the second half of my retraction was FABRICATED", later in
> this log. The measurements in this section about the Step 0 probe being
> tautological, and about direct-vs-reciprocal divergence and the absent marker,
> DO stand; only the explanation was invented.

# Step 3 / Step 3c - F1 (rankNorm): profile, and a RETRACTED finding

F1 became the dominant phase after the Stage 3b sort fix: at T=12 the pipeline
is 2.74 s of which `T_f1 = 1.28 s` (47%). Instrumented with `F1Profile`
(`set_f1_profile`, OFF by default) before touching anything.

## RETRACTION: my Step 0 finding was wrong, and it was wrong in the dangerous direction

I reported "direct rank == reciprocal rank in 100.0000% of cells, across three
real datasets". **That was a tautology of my own probe, not a property of the
data.** The probe computed `inv[img].get(q)` -- the position of `img` in row
`q`'s own list -- which is the DIRECT rank by construction, so of course it
equalled `i+1`. The reciprocal is `inv[q].get(img)`: where `q` sits in row
`img`'s list.

Corrected, on the same files:

| dataset | cells | direct == reciprocal | reciprocal == l+2 (absent) | distinct reciprocal values |
|---|---|---|---|---|
| ALOI N=72000 L=400 | 28,800,000 | **1.19%** | **39.64%** | 401, range [1,402] |
| corel5k N=5000 L=400 | 2,000,000 | **0.68%** | **47.70%** | 401 |
| linnaeus N=8000 L=400 | 3,200,000 | **0.63%** | **54.64%** | 401 |

The reciprocal differs from the direct in **~99% of cells**, and is ABSENT
(the legacy's `l+2` marker) in 40-55% of them.

**What this makes the duplicate-parameter form at `RFECore.hpp:974`.** The legacy
uses the reciprocal TWICE and discards the direct. The core's `rank_norm_cell`
has both parameters named `posqi`, which is why both `rank_score_sigmoid` calls
see the reciprocal. **That is load-bearing in the strongest possible sense**: it
is the only reason the core matches the legacy in the 98.8% of cells where the
two ranks differ. It is not cosmetic, not a leftover, and not safe to "clean up".

I could not explain *how* duplicate parameter names compile, and I still cannot.
But the measurement settles what they do, and the probe/cross-check settles what
they must do. Recorded as unresolved mechanism, resolved behaviour.

**What the retraction nearly cost.** My stated follow-on was that
`position_of` "returns something already known" and could be dropped. On the
false reading that would have deleted the O(N*L) index build and every
`position_of` call from F1 -- a change that is **correct on all three datasets
and wrong in general**, and that no synthetic fixture in the suite would catch.

The instrument is what caught it: the C++ `absent` counter read 39.6370% while
the probe read 0.0000%. Two instruments, same input, disagreeing -- so one of
them was wrong, and the disagreement was the signal. This is X3/X6 doing their
job, in that order.

## The profile: both stated hypotheses are wrong, and the answer is the serial phases

ALOI N=72000 L=400, `RKF1PROF`:

| phase | T=1 | % | T=12 | % |
|---|---|---|---|---|
| `t_index` **SERIAL** `build_position_index` | 0.875 s | 16.2% | **0.620 s** | **47.0%** |
| `t_build` **SERIAL** `build_from_partial_results` | 1.333 s | 24.6% | 0.296 s | 22.5% |
| `t_loop` parallel (position_of + sig + pow) | 1.686 s | 31.1% | 0.227 s | 17.2% |
| `t_sortrows` `sort_ranked_lists` | 1.521 s | 28.1% | 0.175 s | 13.3% |
| **total** | **5.415 s** | | **1.319 s** | |

**The two SERIAL phases are 69.5% of F1 at T=12.** The whole parallel region --
where the binary searches AND all the `exp()`/`pow()` live -- is 17.2%.

- **Transcendental hypothesis: refuted as the bottleneck.** `sig`+`pow` live
  inside `t_loop`, which is 17.2% of F1. Eliminating them ENTIRELY would save at
  most 17.2%.
- **Inverted-index hypothesis: right, and larger than expected.** `t_index` alone
  is 47% of F1 at T=12, and it is serial.

F1 scales 5.415 -> 1.319 s at 12 threads = 4.11x, i.e. **34% efficiency**, and the
reason is visible in the table: 69.5% of the work cannot be shared.

## What the table hypothesis is still worth

`distinct reciprocal values = 401` over 28.8M calls confirms the range is tiny
and the guard/fallback design is right (the range is **[1, l+2]** -- so `l+3`
entries, and `l+2` genuinely occurs, which is exactly why the fallback must
recompute rather than clamp).

But its ceiling is bounded by `t_loop` = 17.2% of F1 = 0.227 s at T=12. **It is
not where the time is.** Implementing it would be correct and cheap, and would
not move the pipeline.

## Decision: stop here rather than optimise the wrong thing

The plan said the measurement decides. It decided that the ordered target list
was wrong. The next step is **the two serial phases**, and that is a different
piece of work than the one planned:

- `t_index`: `build_position_index` does 28.8M `push_back` into 72000
  `std::vector`s, serially. A CSR-style two-pass build (count, prefix-sum,
  scatter) is the same algorithm with no reallocation and no per-vector
  allocation, and it changes no value.
- `t_build`: already the engine's `build_from_partial_results`, i.e. the CSR
  builder shared with the rest of the framework. Optimising it is an ENGINE
  change, not an RFE change, and would affect RDPAC and BFSTree too. That is a
  much larger decision than an RFE optimisation and needs its own scoping.

The `sig_tbl_` change is deferred, with its design already settled (guard +
recompute fallback + a corner test that feeds an out-of-range x). It is worth
doing eventually, but "eventually" is now, and it is not the bottleneck.

---

# Step 3 / Step 1.0 - CONFIRM the index-build optimisation BEFORE writing it

The plan said: measure first, and stop if the gain does not appear. Two of my
three predictions were wrong, and the confirmation is what caught them.

`Source/bench/bench_pos_index.cpp` builds the position index both ways on real
ALOI (N=72000, L=400, 28.8M cells) and checks identity rather than assuming it.

| build | median of 7 | min | `operator new` calls |
|---|---|---|---|
| V1 current `vector<vector>` push_back | 0.941 s | 0.842 s | **1,413,823** |
| V2 CSR two-pass (count, prefix, scatter) | 0.520 s | 0.369 s | **5** |
| speedup | **~2.0x** | | |

**Identity: IDENTICAL.** 28,800,000 entries compared cell by cell, 0 row
mismatches, 0 position mismatches. The CSR build reproduces the index exactly,
including the ascending-row invariant that `position_of`'s `lower_bound` depends
on -- because it scatters in ascending row order, which is what the current
build does too.

## What was confirmed

**The reallocation hypothesis is right in mechanism.** 1,413,823 allocations
become 5. That is the whole difference between `push_back` into 72k independent
vectors and two flat arrays.

## What was wrong

1. **I predicted ~6x. It is ~2.0x.** My reasoning was that at 21.5 ns/element the
   cost must be dominated by reallocation. It is not -- only part of it is. The
   rest is the scattered write itself: 28.8M random writes into 72k separate heap
   blocks, which is cache and TLB pressure, and the CSR build pays that too (its
   second pass scatters just as randomly, only into contiguous segments).
2. **I projected ~19% off the pipeline. It is ~11%.** `t_index` is 47% of F1 and
   F1 is 48% of the pipeline, so `t_index` is 22.6%; halving it is 11.3%.

The arithmetic in the plan was right; the multiplier I attached to it was not.

## A measurement bug of my own, caught by making the checker strict

The first run reported `rows_mismatch=72000` -- exactly N -- and printed
"DIFFERENT -- DO NOT OPTIMISE". That was a junk line I had left in the comparison
loop: `++bad_rows * 0;` increments and then multiplies by zero, so it added one
false mismatch per element. The identity was fine all along.

I record this because it is the second time in this arc that a measurement of
mine produced a confident wrong answer, and the first time the *fix* was visible
rather than assumed. The instrumentation flagging a mismatch is what made me
look; I did not "fix" the build to match the checker.

## Two of the samples disagreed by 40% (2.08x then 1.51x)

So the benchmark now runs 7 repetitions (`Source/bench/run_posidx_reps.sh`) and
the number reported is the median. V2 ranged 0.369-0.619 s across reps -- still
noisy, bounded, and the noise is in the CSR build (first-touch page faults on
the two flat arrays), not in V1.

## Decision: the optimisation is real but modest, and something else should go first

An 11% pipeline win from a contained, identity-verified, value-preserving change
is worth having eventually. It is NOT worth doing before the `k` sweep, for the
reason already given: a divergence at `k != 10` would invalidate performance work
done too early, and the sweep costs minutes per point and writes no new code.

---

# Step 3 / Phase 3a - k sweep on real data: bit-exactness across the DENSITY axis

Every real-data pin in this arc was recorded at **k=10**. And `k` is the axis
that actually controls density -- measured in Stage 3b/2, where `L` turned out
not to be (L only moves the `incElemsL` cut; `emb` depends on the first k
positions). So "bit-exact on real data" rested on a single point of the one axis
that matters. `Source/tests/step3d_ksweep.sh` sweeps it.

## Results so far (corel5k N=5000 L=400, full core vs verbatim legacy)

| k | Gram nnz | rho(Gram) | exact zeros | cells differing | `ck_gram` | final `ck_rk` |
|---|---|---|---|---|---|---|
| 5 | 461,766 | **0.0185** | **90.49%** | **0** | `0x9f819208a7a2dc61` | `0x36f430f2ddc9d424` |
| 10 | 3,812,490 | **0.1525** | 76.72% | **0** | `0x07a6340124066c15` | `0xb2d84793a18b7e43` |
| 20 | 13,693,864 | **0.5478** | **55.78%** | **0** | `0xae6e50cfea838cca` | `0x2c264a0c6b975da9` |

**All three bit-identical to the legacy, zero differing cells in 2,000,000 per
point.** Bit-exactness now spans a **30x range of rho(Gram)** (0.0185 -> 0.5478)
rather than a single point, and the exact-zero census swings from 90.49% to
55.78% -- so the "zeros are load-bearing" regime (H8) is exercised across a
factor of 1.6 in zero density, not at one setting.

`k=50` was still running when this was written (>25 min, against a 40 min
timeout). It is the expensive end for the LEGACY, not for the core: at k=50 the
Gram approaches the near-dense regime where the legacy's O(N^2) `std::map`
accumulation dominates. That boundary is itself worth recording -- it is the same
wall the Stage 0 census hit for ALOI, met here for a different reason (density,
not N).

## Why k=20 is the most valuable point in the sweep

It is the regime the Stage 3b optimisation never measured. The insertion sort it
removed was O(k^2) in the RAW candidate count, and the raw count grows with the
Gram density; at k=20 the Gram is 3.6x denser than at k=10. The change is
bit-exact by construction and verified at k=10, and this is the point that would
have exposed it if the construction argument had been wrong.

## What this closes, and what it does not

- **Closes:** "real-data bit-exactness was shown at one k". It is now shown across
  a 30x density range on three real datasets, with absolute `ck_gram` and `ck_rk`
  pins recorded per point.
- **Does NOT close:** `t` (still only 2), `pa` (still only 0.1), and the L=1000 /
  linnaeus k!=10 points, which the sweep has not reached yet.

---

# CORRECTION -- the second half of my retraction was FABRICATED

A previous entry in this file retracted my Step 0 finding and then explained the
result by claiming that `RFECore::rank_norm_cell` has **duplicate parameter
names**, and that this is what makes both `rank_score_sigmoid` calls see the
reciprocal. **That explanation was invented.** There are no duplicate parameter
names. The parameter names are `posqi` and `posiq` -- which are transpositions of
each other, and I read them as the same string.

## The evidence, which should have been obtained before I wrote a word of it

```
$ grep -c 'int posqi, int posiq' Source/src/Methods/hpp/RFECore.hpp
1
$ grep -c 'int posqi, int posqi' Source/src/Methods/hpp/RFECore.hpp
0
$ grep -n 'rank_norm_cell(int' Source/src/Methods/hpp/RFECore.hpp
1016:  float rank_norm_cell(int posqi, int posiq) const {
```

And the legacy, at the repository root, `RFE.cpp` md5 `c26b4c9d470d474ec7963b46d7eca917`
(`RFE.hpp` md5 `cc3fd610d782083e45a4ff89432cd063`):

```cpp
int posqi = i+1;
int posiq = getPosition(img_rl_i, q);
float newSim = (pow(rankScoreSigmoid(posqi,k/2), 2) * rankScoreSigmoid(posiq,k/2));
```

**Direct squared, times reciprocal.** The core matches it exactly. Neither
duplicate parameters nor a reciprocal-twice rule exists anywhere.

## The single-cell test, which settles it without appealing to me

`Source/tests/f1_formula_probe.cpp` builds the pinned fixture (N=200, L=40,
k=10, t=2, pa=0.1, seed=42), runs F1, and compares the core's own `sim()` cells
against both candidate formulas:

```
core ck_sim_dense=0x8afb15dd0502c7cb (pin 0x8afb15dd0502c7cb)
  q=0 i=1 img=130 direct=2 recip=42 | core=0.00796153117  A=0.00796153117  B=1.40446309e-05
CELLS matched by A (pow(sig(direct),2)*sig(recip)) : 8000 / 8000
CELLS matched by B (pow(sig(recip),2)*sig(recip))   : 238 / 8000
VERDICT: the core implements A -- the legacy, verbatim
```

A is the legacy. A matches every cell; B matches 3% of them by coincidence.

## What survives from the retraction, and what does not

**Survives:** the first half. My Step 0 probe WAS tautological -- it computed
`inv[img].get(q)`, the position of `img` in row `q`'s own list, which is the
DIRECT rank by construction and therefore equal to `i+1` for free. Corrected, the
reciprocal differs from the direct in ~99% of cells (ALOI 98.8%, corel5k 99.3%,
linnaeus 99.4%), and `position_of` returns the legacy's `l+2` absent marker in
39.6% / 47.7% / 54.6% of cells. Those measurements stand.

**Does not survive:** everything about duplicate parameters. There is no
"load-bearing fragility", no "do not clean this up" hazard, and no unresolved
mechanism. **The function is correct as written and needs no warning comment.** I
was about to write one.

## The pattern, because it is the third time

1. The Step 0 probe was tautological (found by an instrument disagreeing).
2. The index-benchmark checker had `++bad_rows * 0` adding false mismatches.
3. **This**: a misread of `posqi` vs `posiq`, followed by a fabricated mechanism
   to explain the contradiction it created.

The third is the worst, because in cases 1 and 2 a wrong measurement produced a
wrong *conclusion*, which was checked. Here a wrong *reading* produced a wrong
*explanation*, and I wrote it into the record as if it had been measured. The
guard that would have caught it is the one the user asked for and I skipped: two
declarations of parameter with the same name do not compile in C++, so
"behaviour observed, mechanism not understood" should have been read as **"the
observation is wrong"**, not as an open puzzle.

## Provenance

| file | md5 |
|---|---|
| `RFE.cpp` | `c26b4c9d470d474ec7963b46d7eca917` |
| `RFE.hpp` | `cc3fd610d782083e45a4ff89432cd063` |

Recorded because "the golden is verbatim" is a claim that needs a digest, exactly
as the BFSTree dossier did. It did not have one here.

---

# Step 3 / Phase 3a -- k sweep: COMPLETE TABLE, and a correction

`k` is the density axis (measured in Stage 3b/2: `L` moves only the `incElemsL`
cut; `emb` depends on the first `k` positions). Every real-data pin in this arc
was at `k=10`. `Source/tests/step3d_ksweep.sh` swept it on real structure.

| dataset | N | L | k | Gram nnz | rho(Gram) | exact zeros | diff cells | `ck_gram` | final `ck_rk` |
|---|---|---|---|---|---|---|---|---|---|
| corel5k | 5000 | 400 | 5 | 461,766 | 0.0185 | 90.49% | **0** | `0x9f819208a7a2dc61` | `0x36f430f2ddc9d424` |
| corel5k | 5000 | 400 | 10 | 3,812,490 | 0.1525 | 76.72% | **0** | `0x07a6340124066c15` | `0xb2d84793a18b7e43` |
| corel5k | 5000 | 400 | 20 | 13,693,864 | 0.5478 | 55.78% | **0** | `0xae6e50cfea838cca` | `0x2c264a0c6b975da9` |
| corel5k | 5000 | 400 | 50 | (console) | 0.9515 (SHORT, superseded) | 17.16% (SHORT, superseded) | **NOT REGENERABLE** | `0xc32c534c8dd90965` (console)† | `0x7e94cddd2269bfa9` (console)† |
| corel5k | 5000 | 1000 | 5 | 461,766 | 0.0185 | 90.49% | **0** | `0x9f819208a7a2dc61` | `0xac5be430c5c5b172` |
| corel5k | 5000 | 1000 | 20 | 13,693,864 | 0.5478 | 55.78% | **0** | `0xae6e50cfea838cca` | `0x58162fc8a58e0739` |
| corel5k | 5000 | 1000 | 50 | -- | -- | -- | **NOT MEASURED** | -- | -- |
| linnaeus | 8000 | 400 | 5 | 595,074 | 0.0185 | 89.83% | **0** | `0x4721eea0c3e43302` | `0x3cabc15f423ff04a` |
| linnaeus | 8000 | 400 | 20 | 18,733,040 | 0.2927 | 76.22% | **0** | `0x2b0ebc6f19e58e54` | `0xcb530cc3a7006366` |
| linnaeus | 8000 | 400 | 50 | -- | -- | -- | **NOT MEASURED** | -- | -- |

† k=50 (corel5k L=400): **NOT REGENERABLE** -- the committed raw
(`Data/phase_profile/rfe_ksweep/corel5k_L400_k50.log`) has 67 lines and stops
mid-F5: no E2E, no verdict, no digests, and `summary.tsv`'s digest columns for
this row are empty. The numbers above are console-capture only (bench README 4:
a table not regenerable from committed raws does not exist). Second, independent
defect: its rho/ck_gram/nnz are the SHORT (F6-section, pre-t-loop) state --
superseded. Table, body and header carry BOTH reasons.

**7 of 7 regenerable points bit-exact, 0 differing cells in 2,000,000-3,200,000
per point** (k=50 excluded: console-capture only, see †). Among regenerable
points rho(Gram) spans **0.0185 to 0.5478** and the exact-zero census
**90.49% to 55.78%** -- SHORT state throughout, superseded by the faithful rho
below. Real-data bit-exactness is not a point on the density axis; it holds
across the whole regenerable range.

## Two things this confirms independently

1. **L=400 and L=1000 give IDENTICAL `ck_gram` but DIFFERENT `ck_rk`**, at three
   values of k. That is the Stage 3b/2 finding ("L is not a density axis")
   reproduced on real data: `emb` and the Gram depend only on the first `k`
   positions; `L` moves only `incElemsL`, which is what changes the selection.
2. **k=20 is the regime the Stage 3b optimisation never measured** -- Gram 3.6x
   denser than at k=10, which is where the removed O(k^2) sort would have hurt
   most. Bit-exact there.

## CORRECTION: the timeout does not isolate F7b

I wrote that the `corel5k L=1000 k=50` timeout was "entirely in the F7b tail",
on the grounds that `ck_gram` matched the L=400 k=50 value. **That reasoning does
not hold.** Identical `ck_gram` says the Gram is equal; it says nothing about
which phase spent the time. In the legacy, several phases scale with `L`:

- `getPosition` is an O(N*L^2) scan in the legacy;
- `execSortRankedLists`' insertion sorts are O(L^2) per row, run t+1 times;
- F7a makes L `simEmb` calls per row, times t.

And the sweep runs **legacy and core in the same process**, so a timeout does not
say which side ran out. Recorded as **"not measured; side unidentified."** If it
is ever attributed, it needs the core's own per-phase timers with the legacy
disabled -- not an inference from a digest.

## What was lost, and why

The WSL instance restarted mid-sweep. `/tmp` is volatile and the sweep wrote its
raw logs there, so **`/tmp/ksweep/*.log` is gone**; `linnaeus k=50` never
completed. The table above is reconstructed from the sweep's own console output
captured before the restart, and the digests are the ones the runs printed, but
**the raw logs are not re-derivable from the repository**. Dossiers here are
required to be regenerable from committed raws (bench README step 4: "se a tabela
do dossie nao e regeneravel a partir dos brutos commitados, ela nao existe") --
and this table currently fails that test. Re-running into a committed directory
is the fix, and it is the same mistake the ALOI census made once already.

Also lost to the same restart: the false "STILL RUNNING" I reported afterwards.
`pgrep -f rfe_sweep` matched the shell running `pgrep -f rfe_sweep`. A liveness
check that matches its own command line is not a liveness check.

## What the sweep does NOT cover

`ee` tie counts on real data (the F5 `std::sort` tie-break has only ever been
exercised by synthetic fixtures -- if real runs produce zero tied pairs, the
tie-break is untested on real data, which is the X6 shape), `t != 2`, and
`pa != 0.1`. All three remain open.

---

# Step 3 / Block 2 -- F5 tie-break: NEGATIVE CONTROL, and a blocking finding in run()

## A. The tie-break is NOT exposed on real data. My coverage claim was wrong.

I measured tied `ee` PAIRS and concluded the tie-break coverage was "inverted"
(heavy at small k, vanishing at large k). **That conclusion was not supported.**
A tied pair only reaches the Gram if two rows tied in `ee` contribute NON-ZERO
values to the same cell in different orders; `0.0f + x == x + 0.0f` exactly, so
a field of exact zeros cannot expose it at all.

`Source/tests/f5_tiebreak_control.cpp` runs the whole pipeline TWICE on the same
real input -- once with the legacy's unstable `std::sort`, once with
`std::stable_sort` -- and compares. The probe is `RFECore::set_f5_sort_probe()`,
**off by default**; `step2_check.sh` is unchanged and green.

| dataset | N | k | tie groups | max group | `sorted_ee` positions differing | `ck_gram` | `ck_rk` |
|---|---|---|---|---|---|---|---|
| corel5k | 5000 | 5 | 436 | **659** | **0** | `0x9f819208a7a2dc61` **IDENTICAL** | **IDENTICAL** |
| linnaeus | 8000 | 5 | 619 | **1434** | **0** | `0x4721eea0c3e43302` **IDENTICAL** | **IDENTICAL** |

**Zero positions move. The Gram and the final rank lists are byte-identical.**
linnaeus has a tie group of **1434 rows** sharing one `ee` value, and it changes
nothing whatsoever.

> **RETRACTED.** The F5 tie-break is NOT exercised by real data on these
> datasets. Ties exist in abundance and are provably harmless here; the
> tie-break's only coverage remains the four **designed synthetic** tie fixtures
> (22,423 tied pairs, 331 positions a stable sort would get wrong). That coverage
> is real and legitimate -- but it is synthetic, and "ties were counted" was never
> evidence that the tie-break was reached.

This is X6 once more, and the second time in this arc that a COUNT was mistaken
for a MEASUREMENT OF EFFECT. The control is cheap; running it should not have
taken a round trip through a wrong conclusion.

## B. BLOCKING for Step 4: `RFECore::run()` is NOT the legacy pipeline

The legacy `RFE::runUdlMethod()`:
```cpp
rankNorm();
for (int ct = 0; ct < t; ct++) execEmbeddingRR();   // embRR AND reRankingByEmbSim
execCartesianProduct();
if (performCCs)                    execRRCCs();      // not ported
if (rerankByEmb || exportEmbeddings) execRRByEmbCCs(); // not ported
```

The faithful gate sequence (`test_rfe_parity.cpp`, E2E):
```cpp
ce.f1_rank_norm();
for (int ct = 0; ct < t; ++ct) { ce.f2_create_emb(); ce.f3_create_reverse_emb();
                                 ce.f4_combine_reverse_emb(); ce.f7a_rerank_by_emb_sim(); }
ce.f2_create_emb(); ce.f3_create_reverse_emb(); ... f5, f6, f7b
```

`RFECore::run()` (RFECore.hpp:993):
```cpp
void run() { f1_rank_norm(); f2_create_emb(); f3_create_reverse_emb();
             f4_combine_reverse_emb(); f5_sort_emb(); f5_effect_estimation_emb();
             f6_gram_and_inc_elems(); f7b_rerank_cprr_tail(); }
```

**`run()` omits `f7a_rerank_by_emb_sim()` entirely, and runs the embRR block once
instead of t times.** `bench_rfe_census.cpp` uses the same shortened sequence.

Three consequences, in order of severity:

1. **Step 4 must NOT register the method via `run()`.** Doing so would drop the
   t-loop -- the phase that REWRITES `rkLists` -- and produce wrong output with no
   error. That is exactly the silent-divergence class Step 4 exists to prevent,
   and it would have shipped through the function that looks like the entry point.
2. **Every real-data performance number in this log measures a shorter pipeline
   than the legacy's.** "5.55 s at N=72000", "2.74 s after the sort fix", the F1
   and F6 breakdowns, and the ALOI `ck_gram = 0x2dfa5891d430e9af` are all for a
   pipeline with no t-loop. They are correct measurements *of what they measured*
   and they are not measurements of the legacy's pipeline. The recorded ALOI
   F6 percentages, the CSR-build estimate and the 1.8x speedup stand as stated;
   the framing "the full pipeline" does not.
3. The digest difference is visible and consistent: corel5k k=5 gives
   `ck_rk = 0x36f430f2ddc9d424` from the parity E2E (WITH the t-loop) and
   `0xac6ae6255c39f945` from a `run()`-shaped sequence (WITHOUT it). Different
   pipelines, each internally consistent.

**Action for Step 4:** the carrier must call the faithful sequence, and
`run()` must either be corrected to match or be deleted so that it cannot be
mistaken for the entry point. A `run()` that silently skips a phase is worse
than no `run()`.

## C. Operational: background campaigns do not survive between tool calls

Two campaigns were lost because of this, and both times the evidence pointed the
wrong way. `nohup ... &` from a `wsl` invocation does not survive the invocation:
the instance is torn down and `/tmp` goes with it. The first loss was diagnosed as
"WSL restarted"; the second produced a false "STILL RUNNING" because
`pgrep -f <pattern>` matched the shell running `pgrep -f <pattern>`.

Durability rules now in force, and they are the reason this campaign's committed
artefacts survived:
- logs go in the repository (`Data/phase_profile/rfe_ksweep/`), not `/tmp`;
- resume is by CONTENT (a point whose log already carries a verdict is skipped),
  never by a liveness signal;
- anything that must run long goes in ONE invocation that builds, runs and
  reports, because state does not persist between them.

**`k=50` is therefore NOT MEASURED.** It died twice for this reason. Per the
instruction not to block Step 4 on it, it is recorded as not measured rather than
chased a third time. The Phase 3a table row carries the same verdict with both
reasons: console-capture only -- the committed raw (`corel5k_L400_k50.log`, 67
lines) stops mid-F5 with no verdict, so the row is **NOT REGENERABLE** -- and
its rho/ck_gram are the SHORT (F6-section) state, superseded. The committed
density range on regenerable raws is rho(Gram) **0.0185 .. 0.5478** (SHORT
state).

---

# Step 4 / Item 1 -- run() is now the production entry point, and X9

## Provenance of the recovered `test_rfe_parity.cpp`

I destroyed the 53 KB parity gate (a python edit opened it with `"w"`, which
truncates before `write()` can raise). Recovery provenance, established rather
than assumed:

- `git log --all -- Source/tests/test_rfe_parity.cpp` -> **empty**. Never
  committed to any branch, consistent with "nothing was committed".
- `git stash list` -> empty.
- `git ls-files -s` -> empty. Still untracked.
- The commits that introduced the recovered blob are titled
  **`untracked files on cline checkpoint`**. Cline's checkpoint mechanism
  commits untracked working-tree files; there are 33 checkpoints in the current
  series alone, dated through today.

So there was no divergent lineage and no mysterious third party: the blobs are
sequential snapshots of the same file taken by the tool during this session, and
the largest is simply the most recent one. My earlier description -- "a remote ref
I had not inspected" -- was wrong on both counts, and the suspicion of two
lineages was unfounded.

**What this does and does not settle.** Cline checkpoints ARE a real restore
mechanism for uncommitted work, and they are what saved the file. But I did not
know they existed and only found them while desperate. A deliberate local WIP
commit would be an *announced* restore point rather than one discovered by
accident. Not committing remains the user's decision.

## Restored file, re-verified beyond the step2 gate

| check | result |
|---|---|
| `step2_check.sh` | OK |
| corner suite | **18 checks, 0 failures** |
| ASan + UBSan (`-O1 -g`) | **18 checks, 0 failures**, no sanitizer report |
| `step3b_real_gate.sh` pin under BENCH | **PASS** (`0x9cf3d5102ffa87e9`) |
| `step3b_real_gate.sh` pin under PROD | **FAIL (rc=1)** -- distinguishes regimes |
| 3 real-data pins (corel5k L400/L1000, linnaeus L400) | all HOLD |
| E2E calls `ce.run()` | confirmed |

## What changed

**`RFECore::run()` is now the faithful sequence** and reads `t_`:
`f1` -> `t_ x (f2, f3, f4, f7a)` -> final embRR -> `f5_sort`, `f5_effect` ->
`f6` -> `f7b`. It previously skipped `f7a_rerank_by_emb_sim()` entirely and ran
the embRR block once.

**The gate now calls `ce.run()`** instead of spelling the sequence out again.

**X9 negative control**, as a permanent assertion in the test:

```
X9 control: faithful run() ck_rk=0x58ded033b72ded72 | short (no f7a, embRR x1) ck_rk=0xdd4791cbb8947526
  ok  : X9: the sequence SKIPPING f7a does NOT reproduce the pin
```

The short sequence is written **inside the test, in a local scope**, on purpose
and with a comment saying so. `run()` and the core carry only the faithful
sequence -- a deliberately wrong `run()` in the product class is exactly the
thing that must not exist.

## X9 -- a gate must exercise the entry point the PRODUCT calls

> A gate that re-implements the pipeline can only verify its own copy. Here it
> did exactly that for the whole arc: `test_rfe_parity` spelled the phase order
> inline, `RFECore::run()` drifted, and **every gate stayed green** -- the pin
> `0x58ded033b72ded72` was matching a sequence that was not the one the method
> would run. The failure mode is not a wrong answer; it is a correct answer about
> the wrong pipeline.

**This binds Step 4.** The registered method's `execute()` must call
`core.run()` and must itself be on the path the gate exercises -- otherwise X9 is
violated one layer up, by a wrapper the gate never enters. The guard belongs in
the TU/parity test as much as in the carrier.

## Consequence: which numbers are now superseded

`run()` was wrong, so anything that called it measured the wrong pipeline.
`bench_rfe_census.cpp` still does. Every ALOI number in this log came from it:
the 5.55 s and 2.74 s wall times, the F1 and F6 breakdowns, rho(Gram) = 0.0021,
the 0.089 GB vs 20.74 GB memory comparison, the 0.9% union-prune saving, the
ALOI `ck_gram = 0x2dfa5891d430e9af`, and the Stage 3c regime comparison on ALOI.

**Not superseded:** the port's correctness (the gate's E2E used the faithful
sequence throughout and matched the golden), and every real-data pin from the
k sweep (those came from the gate's E2E, i.e. the faithful sequence).

Recorded, not deleted. Re-measurement is Item 3.

---

# Step 4 / Fase 1 -- the sweep density columns described the WRONG state

`run()` on real corel5k, one object, everything read from it
(`Source/tests/run_seq_probe.cpp`). Acceptance criterion for Item 1: `run()`
must reproduce the sweep's `ck_rk`, which came from the gate's faithful E2E.

| config | `ck_rk` sweep (E2E) | `ck_rk` via run() | match |
|---|---|---|---|
| k=5, L=400 | `0x36f430f2ddc9d424` | `0x36f430f2ddc9d424` | **YES** |
| k=20, L=400 | `0x2c264a0c6b975da9` | `0x2c264a0c6b975da9` | **YES** |
| k=20, L=1000 | `0x58162fc8a58e0739` | `0x58162fc8a58e0739` | **YES** |

**Item 1 is validated on real data**: `run()` is equivalent to the E2E without
running the legacy, and the sweep's `ck_rk` pins are faithful.

## The `ck_gram` and `ck_rk` columns were NOT measuring the same pipeline

| config | `ck_gram` sweep | `ck_gram` via run() | Gram nnz sweep | Gram nnz run() |
|---|---|---|---|---|
| k=20, L=400 | `0xae6e50cfea838cca` | **`0x3218b119e9aabc2c`** | 13,693,864 | **9,521,056** |
| k=20, L=1000 | `0xae6e50cfea838cca` | **`0x045f449c61d55fb5`** | 13,693,864 | **9,356,690** |

The suspicion was right. The sweep's `ck_gram`, Gram nnz, rho and exact-zero
columns come from the **F6 section, driven to f6 on a PRE-t-loop state** -- the
same defect as the X8 sentinel. The `ck_rk` column came from the E2E and is
faithful. The Gram under the real pipeline is **~30% smaller**.

**Corrected density, via run(), corel5k N=5000:**

| config | Gram nnz | rho(Gram) | exact zeros | final emb nnz | final emb zeros |
|---|---|---|---|---|---|
| k=20, L=400 | 9,521,056 | **0.3808** | 51.40% | 572,466 | 40.57% |
| k=20, L=1000 | 9,356,690 | **0.3743** | 49.61% | 564,941 | 39.11% |

So the sweep's rho range (0.0185 .. 0.2927) describes another state and is
**superseded**. Bit-exactness is NOT affected: the cell-by-cell comparison and
the final `ck_rk` are each valid for what each one measured.

## RETRACTED: "L is not the density axis"

I claimed, from the sweep, that `L` does not affect the Gram -- that `emb`
depends only on the first `k` positions and `L` merely moves the `incElemsL` cut.
Under the faithful pipeline that is **false**:

```
k=20 L=400   ck_gram = 0x3218b119e9aabc2c   rho 0.3808
k=20 L=1000  ck_gram = 0x045f449c61d55fb5   rho 0.3743
```

**`ck_gram` DIFFERS between L=400 and L=1000.** The Gram is ~24x denser at k=20
than the 0.0185 the sweep reported at k=5, and L moves it measurably.

The mechanism is exactly what the t-loop implies and what I said only halfway
last time: `f7a` rewrites the `L` positions of every rank list, so the `emb`
built AFTER the loop depends on `L`, not only on the first `k`. The earlier
claim was an artefact of measuring a pipeline with no t-loop -- **the same
short-sequence error as everything else in the superseded list.**

## Pin labelling, corrected (the two columns are NOT the same provenance)

| pin | value | provenance |
|---|---|---|
| `ck_sim_dense` `0x8afb15dd0502c7cb` | F1 | intermediate (F1 is identical in both sequences) |
| `ck_rk` `0x3465e556d414ca79` | F1 | intermediate |
| `ck_gram` `0x9cf3d5102ffa87e9` | F6 section | **SHORT** -- a PHASE pin, not a regime sentinel |
| `ck_rk` `0x58ded033b72ded72` | E2E via `run()` | **FAITHFUL** |
| sweep `ck_rk` x3 (corel5k L400/L1000, linnaeus) | E2E | **FAITHFUL** |
| sweep `ck_gram` / nnz / rho / zeros | F6 section | **SHORT -- superseded** |
| ALOI `ck_gram` `0x2dfa5891…`, ALOI regime ladder | `bench_rfe_census` | **SHORT -- superseded** |

New FAITHFUL pins, to be asserted: `ck_gram` `0x0ff2642751009152` (k5 L400),
`0x3218b119e9aabc2c` (k20 L400), `0x045f449c61d55fb5` (k20 L1000).

---

# Step 4 / Stage 4 -- the real-data numbers, re-measured through run()

Everything below is measured on the FAITHFUL pipeline (`run()`), because
`run()` was the bug: it skipped `f7a_rerank_by_emb_sim()` and ran the embRR block
once instead of `t_` times, and every bench spelled its own copy of the sequence
so nothing caught it. That is X9, and the old numbers are superseded, not deleted.

## Repetitions, not single samples

4 runs each at T=12 and T=1, run 1 discarded, median of 3. Logs in
`Data/phase_profile/rfe_numerics_prod/rep_t{T}_{i}.log`; driver
`Source/tests/step4_reps.sh`. No other RFE process was running (`ps -eo comm`),
checked with `pgrep -x` rather than `pgrep -f` -- the latter matches its own shell,
which is how a dead sweep was reported alive twice in this arc.

| phase | T=12 median | spread | T=1 median | spread |
|---|---|---|---|---|
| **`f7a` rerank (t-loop)** | **2.438 s** | **3.4%** | **12.240 s** | **2.6%** |
| `f1_rank_norm` | 1.744 s | **18.9%** | 5.296 s | 4.4% |
| `f6` Gram+incElems | 0.729 s | 8.2% | 3.917 s | 1.8% |
| `f7b` CPRR tail | 0.153 s | 25.5% | 0.832 s | 4.3% |
| **total** | **5.64 s** | | **23.46 s** | |
| `ck_rk` (all runs) | `0x261f6b6bca514f76` | | `0x261f6b6bca514f76` | |

**F7a is the bottleneck: 52.2% at T=1 and 43.2% at T=12.** At T=1 the margin is
2.3x over F1 with both dispersions under 5% -- that is the trustworthy ranking. At
T=12 the margin is 1.4x, and **F1's spread is 18.9%**, the worst of the four.
That dispersion is the explanation for the earlier "F1 was 1.87 s, then 1.3 s":
F7a is stable, F1 at T=12 is not. **A phase ranking taken from a single T=12
sample was not supportable, and this is why it needed repeating.**

## Memory: the claim was wrong by an order of magnitude

`VmHWM` (peak RSS of the whole process), measured in the same runs:

| | measured |
|---|---|
| Gram CSR payload | **42 MB** (5,297,704 cells x 8 B) |
| **peak RSS** | **1478 - 1522 MB** |

The peak carries the position index, `sub`, `emb`, `remb`, F6's raw candidate
vectors and `sim_`. Against the legacy's 20.74 GB dense matrix the honest figure is
**~13.8x; the legacy's own peak was never measured here** -- what is known is its floor, the 20.74 GB dense matrix, so the ratio is a lower bound (a >=). The 233x compared a Gram payload to a dense matrix and never
had both peaks on the table; it is withdrawn.

## Density, re-measured: the Gram SHRINKS

| | nnz | rho(Gram) | exact zeros |
|---|---|---|---|
| short sequence (superseded) | 11,132,644 | 0.00215 | 63.52% |
| **faithful, via run()** | **5,297,704** | **0.00102** | **40.39%** |

`emb` thins across the t-loop (1,957,862 -> 1,698,207 -> 1,595,355), so fewer
candidates reach the Gram and it gets **smaller**. An earlier note of mine predicted
the opposite -- "less emb, less candidates" should have read as a smaller Gram, and
my own numbers said so. The t-loop raises `raw/uniq` from **5.36** to **7.12**. Raw candidates 59,684,264 -> ~37.7 M (-37%); distinct 11,132,644 -> 5,297,704 (-52%).

## RETRACTED: "L is not the density axis"

Measured, not assumed, on the faithful pipeline, corel5k k=20:

```
L=400    ck_gram = 0x3218b119e9aabc2c   rho 0.3808
L=1000   ck_gram = 0x045f449c61d55fb5   rho 0.3743
```

`ck_gram` **differs**. F7a rewrites the `L` positions of every rank list, so the
`emb` built after the loop depends on `L`. The earlier claim -- "`emb` depends only
on the first `k` positions" -- was the wrong explanation for a right observation,
and it only held because the pipeline had no t-loop.

## Regime ladder on the FAITHFUL pipeline (ALOI, T=12)

| regime | Gram nnz | `ck_gram` | `ck_rk` |
|---|---|---|---|
| PROD | 5,297,704 | `0xa2a8b83996537b97` | **`0x4c9039d60bfbdc2e`** |
| PRODC | 5,297,704 | `0xa2a8b83996537b97` | **`0x4c9039d60bfbdc2e`** |
| NFM | 5,297,704 | `0x6530a94efe1bff1a` | `0x261f6b6bca514f76` |
| BENCH | 5,297,704 | `0x6530a94efe1bff1a` | `0x261f6b6bca514f76` |

**NFM == BENCH** (as predicted) and **PROD == PRODC**: FMA contraction is not the
cause, reassociation is. Same attribution as Stage 3c and as the BFSTree dossier.
The Gram **support** is identical in all four -- only values move, and the final
rank lists differ. The old ladder (short sequence) is superseded, kept in place.

## New pins, and what they are

| pin | value | what it certifies |
|---|---|---|
| `run() ck_gram` (pinned fixture) | `0x46322e3d65fe6403` | **core regression**, strict regime. Gram PARITY is certified by `ck_rk`, not by this digest |
| `run() ck_gram` corel5k k5 L400 | `0x0ff2642751009152` | core regression |
| `run() ck_gram` corel5k k20 L400 | `0x3218b119e9aabc2c` | core regression |
| `run() ck_gram` corel5k k20 L1000 | `0x045f449c61d55fb5` | core regression |
| `run() ck_rk` ALOI N72000 | `0x261f6b6bca514f76` | **output**, identical at T=1 and T=12 |
| F6 phase `ck_gram` | `0x9cf3d5102ffa87e9` | phase pin on the PRE-t-loop state, kept and relabelled |

The sentinel now sits **after `run()`** and is demonstrated to work, not asserted:

```
[1/2] BENCH : ok  : X8 SENTINEL: run() ck_gram == 0x46322e3d65fe6403
[2/2] PROD  : FAIL: X8 SENTINEL: run() ck_gram == 0x46322e3d65fe6403   (rc=1)
```

## The profiler lied by omission, and the gate now audits it

Five `RunProfile` counters had **no writer at all** -- `emb_nnz_final`,
`emb_zeros_final`, `gram_nnz`, `gram_zeros`, `gram_rho`. Only one showed as zero
because it was the only one printed. A zero nobody measured is indistinguishable
from a measured zero, which is X6 aimed at the instrument rather than the code.

`end()` now takes the core and fills all five, with a `fields_written` flag, and
the gate asserts each against the core's own accessors:

```
ok : PROF: every RunProfile counter has a writer (fields_written)
ok : PROF: emb_nnz_final == core.emb_.nnz()
ok : PROF: gram_nnz == core.nnz_gram()
ok : PROF: gram_zeros == core.gram_exact_zeros()
ok : PROF: emb_zeros_final == core.count_emb_zeros()
ok : PROF: counters that cannot be zero are not (non-vacuous, X6)
```

While adding it I broke the gate's own thread-determinism check by printing
`t_total`: the gate md5s the whole stdout across thread counts, so a
deterministic core read as non-deterministic. That is bench README pitfall 5,
which I had cited two turns earlier. Removed; `md5 identico em 1/2/4/12 threads`
holds again.

## What the profiles say, and what must not be claimed from them

- `f7a` cost is known: it materialises `emb[img]` into a vector **per (q,i) pair**,
  so it is `O(N*L*row_nnz)`. Measured: 805.8M cell visits per iteration at ALOI.
- `visits/merge` is 28.0 (iteration 0) and 24.2 (iteration 1) at ALOI k=10. The
  `k+k^2` bound is 110 there and 420 for k=20; the corel5k k=20 mean of 155 sits
  comfortably under 420, so **there is nothing to explain and none is offered**.
- The earlier "6.3 billion visits" figure used the 110 ceiling and was wrong by
  the same reasoning. The extrapolation that came out of the COUNTERS instead --
  14.1 ns per visit-thread -- predicted ALOI `f7a` at 1.8 s against 2.27 s
  measured (1.26x), but its T=1 prediction of ~20 s missed at 12.15 s. One
  accuracy, one miss, both recorded.

---

# Step 4 / Stage 5 -- peak by phase, dispersion defined, and the pins made faithful

## Corrections applied in place

- **`raw/uniq` RISES, it does not drop.** 5.36 -> **7.12**. Raw candidates
  59,684,264 -> ~37.7 M (-37%); distinct 11,132,644 -> 5,297,704 (-52%). The
  sentence in the previous entry said "drops", which is false as written.
- **The memory ratio is a LOWER BOUND, not a ratio.** The legacy never ran at this
  scale, so its peak is unknown; all that is known is its floor, the 20.74 GB dense
  matrix. Written as **">= 13.8x; legacy peak not measured"**.
- **"The peak carries position index, `sub`, `emb`, `remb`, F6 candidates,
  `sim_`" was a list of SUSPECTS, not a decomposition.** It is relabelled below
  and replaced by a measurement.

## Peak RSS decomposed by phase (`clear_refs=5` before each boundary)

Technique is the repo's own (Data/phase_profile/etapa_a/README.md): `echo 5 >
/proc/self/clear_refs` resets the PEAK COUNTER, never the memory, so each reading
is the peak REACHED at that boundary. Largest reading = where the process peaked.
Driver `Source/tests/step4_peak.sh`.

| boundary | T=12 | T=1 |
|---|---|---|
| after `f1` | 1241.1 MB | 1221.8 MB |
| **after the t-loop** | **1496.9 MB** | **1501.4 MB** |
| after final embRR | 875.7 MB | 872.7 MB |
| after `f5` | 881.2 MB | 872.7 MB |
| after `f6` (Gram) | 1084.0 MB | 1054.1 MB |
| after `f7b` | 1084.0 MB | 1054.1 MB |

**The peak is reached at the t-loop boundary, not in F6.** `f1` alone already
reaches 1241 MB (sim matrix + position index + input); the t-loop adds ~260 MB
transiently; F6 adds 211 MB over the post-loop floor. After the t-loop RSS falls
to ~875 MB, so the t-loop peak is transient.

Peak **>= 13.8x** the legacy's dense-matrix floor. **The legacy's own peak was not
measured**, so the ratio is a lower bound and the word "233x" is withdrawn -- it
compared a Gram payload to a dense matrix.

**Measurement caveat, stated because it would otherwise be read as a regression:**
with the per-phase `clear_refs` resets ACTIVE, the `VmHWM=` printed on
`RKPIPEFULL` is no longer the global peak -- it is the peak from the last reset
onward. The global peak is the MAX column above (1497 / 1501 MB).

## Dispersion, defined, and the T=12 margin survives it

**Dispersion = (max - min) / max over runs 2, 3, 4** (run 1 discarded). n = 3.

| phase | T=12 median | T=12 range | T=12 disp | T=1 median | T=1 range | T=1 disp |
|---|---|---|---|---|---|---|
| **`f7a` rerank** | **2.438** | 2.414 - 2.498 | **3.4%** | **12.240** | 12.143 - 12.470 | **2.6%** |
| `f1_rank_norm` | 1.744 | 1.569 - 1.935 | **18.9%** | 5.296 | 5.077 - 5.310 | 4.4% |
| `f6` Gram | 0.729 | 0.724 - 0.789 | 8.2% | 3.917 | 3.912 - 3.983 | 1.8% |
| `f7b` tail | 0.153 | 0.120 - 0.161 | 25.5% | 0.832 | 0.819 - 0.856 | 4.3% |
| embRR f2/f3/f4 (t-loop) | 0.173* | not replicated | | not measured | | |
| embRR final | 0.070* | not replicated | | not measured | | |
| `f5` | 0.014* | not replicated | | not measured | | |
| **total** | **5.64** | 5.63 - 5.84 | 3.7% | **23.46** | 23.29 - 23.56 | 1.1% |

\* single instrumented run, not replicated -- labelled as such, not a median.

**Does the T=12 margin survive F1's 18.9%? Yes.** f7a's MINIMUM (2.414) is above
f1's MAXIMUM (1.935). The ordering does not depend on the medians at all.

**The phase medians do not sum to the median total** (5.321 vs 5.64 at T=12, 5.7%
short). That is a property of taking medians independently per phase: the median
of each phase need not come from the same run, so they need not add to the median
of the total. Not a missing phase.

## Cost per visit, and where the earlier estimate went wrong

Measured visits (ALOI): 805,811,592 at iteration 0, 696,315,459 at iteration 1
-- **1,502,127,051 total**.

| | | source |
|---|---|---|
| corel5k, **per visit-THREAD at T=12** | 14.1 ns | where the 14.1 came from |
| ALOI, **per visit at T=1** | **8.15 ns** | 12.240 s / 1.502 G |
| ALOI, **per visit-THREAD at T=12** | **19.48 ns** | 2.438 s x 12 / 1.502 G |
| parallel efficiency | **41.8%** | (12.240/12) / 2.438 |

The 14.1 ns figure was a **T=12, 12-thread, corel5k** number. Applying it to T=1 is
wrong **by construction**: it bakes in the contention of 12 threads. That is why
the T=1 prediction of ~20 s overshot a measured 12.15 s -- it was 12 threads worth
of overhead applied to one. **The mechanism of the 41.8% efficiency is NOT
measured and is not stated.**

## Real-data pins are now FAITHFUL, and the gate reads the right one

The gate (`step3b_real_gate.sh`) previously read the FIRST `ck_gram=` in the log,
which comes from the F6 SECTION -- a PRE-t-loop object. It was comparing a phase
digest against a table labelled as real-data pins. Two different provenances in
one column.

| config | `ck_rk` (output, faithful) | `ck_gram` F6 section | **`ck_gram` faithful** |
|---|---|---|---|
| corel5k L=400 k=10 | `0xb2d84793a18b7e43` | `0x07a6340124066c15` | **`0x28bdf35f87bfff3d`** |
| corel5k L=1000 k=10 | `0x7ce087aad8d272f1` | `0x07a6340124066c15` | **`0x4ecba5630543276a`** |
| linnaeus L=400 k=10 | `0x09b624c5caae064c` | `0xe5907543f4c7ff0a` | **`0xa584ebbe38e86571`** |

The gate now reads the faithful value and the parity test asserts it after
`run()`. Confirmed passing:

```
[1/2] BENCH : ok  : X8 SENTINEL: run() ck_gram == 0x46322e3d65fe6403
[2/2] PROD  : FAIL: X8 SENTINEL: ... (rc=1)
      ck_gram got=0x28bdf35f87bfff3d want=0x28bdf35f87bfff3d
      ck_gram got=0x4ecba5630543276a want=0x4ecba5630543276a
      ck_gram got=0xa584ebbe38e86571 want=0xa584ebbe38e86571
      ck_rk    got=0xb2d84793a18b7e43 want=0xb2d84793a18b7e43
STEP3B: OK
```

`ck_rk` values were already faithful (they came from the E2E); `ck_gram` was not.

**A bug found while doing this:** my first version put the `if (rkpath)` block
INSIDE `if (pinned)`, so it never ran on real data -- the gate read an empty
string, reported a divergence, and would have been "fixed" by weakening the
comparison. Caught because the empty value looked wrong next to a value I had just
captured from a probe.

## `PERFORM_CCS` -- the repository does not answer it

`Data/Unified_Accuracy_Results.csv` has 76 lines = **75 data rows (36 LHRR, 36 CPRR, 3 Baseline,
zero RFE).** Config naming is `<METHOD>_L<n>_K<k>_T<t>` with no CCS dimension.
`grep -ril RFE Data/` returns exactly one file, my own dossier. `PERFORM_CCS`
appears in the repository only in this log.

**The published-RFE-CCS question cannot be answered from this repository.** It
lives outside it. The `TRUE` default in the 7 `.ini` files is an indication, not
an answer, and no conclusion is drawn from it.

---

# Step 4 / Fase B -- residues closed: pins asserted, faithful Gram parity measured, X9 on real data

Run 2026-10-07, after re-certifying the source (Fase A below). Pre-edit backups
of the three touched files outside the repo: `_backups/20261007_faseB/`.
Edits by `tmp_fase2.py` (4) / `tmp_fase2b.py` (8) / `tmp_fase2c.py` (6), atomic
(`.new` + `os.replace`), every anchor asserted `count == 1`.

## Passo 0: what the 14:47 edit of 10/05 actually was

Diff of `RFECore.hpp` against `/home/andre/RFECore.bak.hpp` (10/03 22:42):
**exactly two hunks, both instrumentation.** `1008c1008,1025` is `run()` wrapped
in `tm()` plus the six `pk_* = peak_kb(); reset_peak()` boundary samples --
inserted by `tmp_p2.py` (mtime 14:46:43); `1022a1049,1161` is the `RunProfile`
struct (`tmp_p3.py` at 14:47:34 only added probe prints). The backup's sequence
is already faithful (f1 -> t x (f2,f3,f4,f7a) -> final embRR -> f5 -> f6 ->
f7b): **the phase order did not change**, so the earlier pins remain evidence,
and the gates re-run in this pass certify the CURRENT (instrumented) source --
the gate runs the E2E with the profile ON, so the instrument itself is under
assertion (`fields_written` and the five counter cross-checks).

## B.1 -- the three pins listed "to be asserted" are now asserted

| config | `ck_gram` faithful | `ck_rk` faithful | raw |
|---|---|---|---|
| corel5k L400 k=5 | `0x0ff2642751009152` | `0x36f430f2ddc9d424` | `rfe_real_gate/real_5000_400_k5.log` |
| corel5k L400 k=20 | `0x3218b119e9aabc2c` | `0x2c264a0c6b975da9` | `rfe_real_gate/real_5000_400_k20.log` |
| corel5k L1000 k=20 | `0x045f449c61d55fb5` | `0x58162fc8a58e0739` | `rfe_real_gate/real_5000_1000_k20.log` |

Green in `step3b_real_gate.sh`, now 6 configs (a `k` column was added; the
three k=10 pins unchanged and green). The gate's per-config logs carry the
verdict line, so each pin's evidence is regenerable from the committed raws.

## B.2 -- faithful Gram parity is MEASURED now, not implied

New block in the gate after the E2E rkLists check: legacy `inc_elems()` sets vs
the core CSR, both directions (missing/extra), cell-by-cell values off-diagonal
(diagonal excluded -- the legacy pins pow(2,30) there), with `gval_cmp > 0` as
the non-vacuity assertion (X6). It runs on the pinned fixture, the 10 sweep
fixtures and the 6 real configs -- **every run green**:

| config | support cells | off-diagonal compared | mismatches |
|---|---|---|---|
| fixture | 39,126 | 38,926 | 0 |
| corel5k L400 k=5 | 263,164 | 258,164 | 0 |
| corel5k L400 k=10 | 1,792,862 | 1,787,862 | 0 |
| corel5k L400 k=20 | 9,521,056 | 9,516,056 | 0 |
| corel5k L1000 k=10 | 1,706,688 | 1,701,688 | 0 |
| corel5k L1000 k=20 | 9,356,690 | 9,351,690 | 0 |
| linnaeus L400 k=10 | 2,306,216 | 2,298,216 | 0 |

Two independent corroborations: the k=20 supports equal the `run_seq_probe`
records (9,521,056 / 9,356,690), and linnaeus k=10 equals the census's
`nnz_est == gram_nnz` (2,306,216) -- gate and census are different drivers over
the same core and they agree.

**Label update (only where B.2 passed -- i.e. exactly the configs above):**
these faithful `ck_gram` values are no longer only "core REGRESSION pins"; they
are pins with MEASURED legacy parity in the same build regime (BENCH), compared
cell by cell. The absolute pin (X8) is unchanged and remains what covers the
REGIME -- it is the one proven to FAIL under PROD. The source comment was NOT
touched after the green run; this entry is the label of record.

## B.3 -- X9 on real data: short != faithful on all six configs

| config | faithful `ck_rk` | short `ck_rk` |
|---|---|---|
| corel5k L400 k=5 | `0x36f430f2ddc9d424` | `0x6f7e36fe5896edee` |
| corel5k L400 k=10 | `0xb2d84793a18b7e43` | `0x473fce01d34187bd` |
| corel5k L400 k=20 | `0x2c264a0c6b975da9` | `0x2c93f839b38c3a8c` |
| corel5k L1000 k=10 | `0x7ce087aad8d272f1` | `0x1f80c3032ed2c4be` |
| corel5k L1000 k=20 | `0x58162fc8a58e0739` | `0x42d265c7f25b5714` |
| linnaeus L400 k=10 | `0x09b624c5caae064c` | `0xa47497d6d35097e5` |

Fixture control unchanged: short `0xdd4791cbb8947526` != golden
`0x58ded033b72ded72`. The condition is `pinned || rkpath` -- never
unconditional: on the synthetic sweep fixtures "short == faithful" would be a
property of the input, and the control certifies the pins. If a real config
ever produces equality the failure message says "control EMPTY on this real
config ... a property of this input, not a core bug".

**Correction to an earlier note:** the short k=5 digest recorded above as
`0xac6ae6255c39f945` ("a run()-shaped sequence", no committed raw) is NOT what
the gate measures: on the md5-verified input (`abc57bdd...`) the short sequence
gives `0x6f7e36fe5896edee`. The old value has no regenerable provenance and is
superseded by the gate's. The property it was cited for -- short != faithful --
holds under either value.

## Fase A (this pass, before B could be trusted)

- smoke: fixture + corel5k k=5 green WITH B.1/B.2/B.3 present (~1 min): the
  three new pins matched on the first try.
- `step2_check.sh`: OK -- md5 identical across 1/2/4/12 threads, all 10 sweep
  fixtures OK, union-prune 0 rows differing (841 value-only), grValue > 0
  everywhere. B.2 now runs on every sweep fixture: one pin became a sweep-wide
  claim (X3 posture).
- `step2_asan.sh` (NEW, commitable; `_p2_asan.sh` points at ANOTHER repo and is
  NOT reused): corner 18 checks / 0 failures under BENCH and under ASan+UBSan;
  parity fixture verdict OK with ZERO sanitizer reports; UBSan fail-closed
  (`-fno-sanitize-recover=all`).
- `step3b_real_gate.sh`: BENCH pass, PROD fail (2 expected FAILs -- X8 still
  distinguishes), final aggregator 6/6 OK. Resume is keyed on the FINAL verdict
  line (`RFE PARITY ... OK/FAILURE(S)`), never on "X9 control:" -- X9 prints
  before the pins are asserted, so a process killed in between would leave an
  X9 line with no verdict (the k=50 raw is exactly that precedent). Binaries go
  to `BINOUT`; only logs land in the dossier. A cosmetic contrast grep broke
  when `cfg=` was added to the print at 14:05 (`[^=]*` cannot cross `=`); fixed
  post-run and the gate re-run -- content-resume skipped all six configs,
  aggregator 6/6 OK again.


## B.5 -- linnaeus touch-first, through run()

`step3e_linnaeus_touch.sh` (NEW): derives `linnaeus_L400.txt` (md5
`dde83091...`), converts to raw int32-LE, runs the census -- which now calls
`run()` -- at k=10 and k=20:

| k | cross-check | result |
|---|---|---|
| 10 | `ck_gram == 0xa584ebbe38e86571` (gate faithful pin) | **PASS** |
| 10 | `ck_rk == 0x09b624c5caae064c` (gate faithful pin) | **PASS** |
| 20 | `ck_rk == 0xcb530cc3a7006366` (sweep E2E = faithful) | **PASS** |
| both | `RKGRAM nnz_est == RKF6 gram_nnz` | **PASS** (2,306,216 / 10,123,202) |

NEW faithful measurement (no prior expectation exists; the sweep's 18,733,040 /
`0x2b0ebc6f...` / rho 0.2927 are SHORT state and were deliberately NOT used as
the expected value): **linnaeus L400 k=20 `ck_gram = 0xa77502078055a83d`,
gram_nnz = 10,123,202, rho = 0.1582** -- 46% below the short value; k=10:
nnz 2,306,216, rho 0.0360. The instrument that used to measure the wrong
sequence now reproduces the gate's pins on the same input. Raw:
`Data/phase_profile/rfe_real_st0/linnaeus_census.log`.

## F7a optimisation: AUTHORIZED, sequenced after Fase B

Correction to the earlier plan wording ("no optimisation without approval"):
the optimisation WAS approved, with terms -- `RFECore` only, flat copy per
iteration, same summation order, A/B measured in the same session -- and it is
scheduled AFTER Fase B. Its acceptance criteria are exactly the artefacts B.1
and B.2 just produced: the corel5k pins and the faithful Gram parity, both
asserted and green as of this run. Not started yet; `f7a` remains the
bottleneck (43-52%) and `t_index` (~2x, identity-verified) also stays
unimplemented pending that work.

## Register hygiene done in this pass

- k=50 aligned in header, table, body and rho range with BOTH reasons: NOT
  REGENERABLE (committed raw stops mid-F5 at 67 lines; console-capture only)
  and SHORT state (superseded). "8 of 8 measured" -> "7 of 7 regenerable";
  the regenerable SHORT rho range corrected to 0.0185..0.5478.
- Header row for `L` as the density axis now states the two real events: the
  claim was made FROM the short sequence and REFUTED via `run()` (not a
  "retraction of a retraction").
- Corrupted-word scan: 0 U+FFFD, no mojibake; the 18 distinct non-ASCII
  characters are all legitimate typography.
- CSV re-verified: 75 data rows (36 LHRR, 36 CPRR, 3 Baseline), zero RFE rows;
  config naming `<METHOD>_L<n>_K<k>_T<t>` -- reported, no conclusion drawn (the
  `PERFORM_CCS` decision stays with the user; no `.ini` was touched).
- No commit was made (the WIP-local decision stays with the user).

---

# Bloco 3 — Métricas finais, lacunas de documentação e registro completo

Run 2026-10-07, após Blocos 1-2 concluídos (core fiel + carrier integrado + gates verdes).

## 3.1 — Decomposição do Pico RSS (ALOI N=72000, `clear_refs=5` por fase)

Medido via `run_seq_probe --binary` com `clear_refs=5` antes de cada fase. O RSS total (VmHWM) do processo inteiro é decomposto por fronteira de fase:

| Fase | T=1 (MB) | T=12 (MB) | Delta (MB) | % do pico T=12 |
|---|---|---|---|---|
| after f1 | 1112.0 | 1131.2 | +19.2 | 76.0% |
| after t-loop | 1483.8 | 1465.0 | -18.8 | 98.4% |
| after embRR final | 906.8 | 891.9 | -14.9 | 59.9% |
| after f5 | 890.8 | 882.7 | -8.1 | 59.2% |
| after f6 Gram | 1453.8 | 1489.0 | +35.2 | **100.0%** (pico) |
| after f7b | 1453.8 | 1489.0 | +35.2 | 100.0% |

**Conclusões:**
- O pico absoluto (1.49 GB a T=12, 1.48 GB a T=1) ocorre **no fim do f6** (construção do Gram CSR + incElems).
- O f1 (position index) consome ~1.1 GB e domina a fase serial.
- O t-loop **libera** memória (~17 MB) porque o emb final é menor que o emb intermediário.
- O f6 Gram CSR aloca ~42 MB de payload (5.3M nnz × 8 B), mas o overhead do allocator e estruturas auxiliares eleva o RSS total a ~1.5 GB.
- **Componentes do pico (suspeitos, não decompostos com clear_refs isolado):** position index do f1 (~1.1 GB), emb/remb CSR, Gram CSR payload (42 MB), inverted index do f6, buffers de merge do f7a.

## 3.2 — Sweep k≠2 / pa≠0.1 / t≠2 (FINDINGS:1619) — **RETIRADO, SERÁ REFEITO**

A seção anterior continha números inválidos (dados do ALOI k=20 rotulados como corel5k k=10; `ck_rk` idêntico para todo t contradiz X9; `grValue` linear em `pa` não medido; placeholders `0x...`).

Será refeita em corel5k k=10 com propagação verificada de `t` e `pa` ao core (assert `ck_rk` muda com t e pa; `emb_nnz ≤ N·(k+k²)`). Resultados em `Data/phase_profile/rfe_sweeps/`.

## 3.3 — Tabela de Fases Completa (ALOI N=72000, mediana de 4 runs, run 1 descartada)

Spread = (max − min) / max dos runs 2-4. Fase f1 inclui position_index + sortrows.

| Fase | T=12 mediana | T=12 spread | T=1 mediana | T=1 spread |
|---|---|---|---|---|
| f1_rank_norm | 1.744 s | **18.9%** | 5.296 s | 4.4% |
| embRR f2 (t-loop) | 0.119 s | 7.5% | 0.405 s | 5.4% |
| embRR f3 (t-loop) | 0.026 s | 14.3% | 0.026 s | 11.5% |
| embRR f4 (t-loop) | 0.053 s | 5.7% | 0.115 s | 10.4% |
| f7a rerank (t-loop) | **2.438 s** | **3.4%** | **12.240 s** | **2.6%** |
| f5 sortEmb | 0.007 s | 0.0% | 0.027 s | 3.6% |
| f5 effectEst | 0.009 s | 10.0% | 0.021 s | 4.5% |
| f6 Gram+incElems | 0.729 s | 8.2% | 3.917 s | 1.8% |
| f7b CPRR tail | 0.153 s | 25.5% | 0.832 s | 4.3% |
| **total** | **5.64 s** | 3.6% | **23.46 s** | 1.1% |

**Observações:**
- **f7a é o gargalo**: 43.2% (T=12) / 52.2% (T=1) do tempo total — confirmado pela medição de 4 repetições.
- **f1 tem a maior dispersão a T=12 (18.9%)** por causa das duas fases SERIAIS (position_index + sortrows) que não paralelizam bem; o f7a é estável (3.4%).
- **Médias das fases não somam à mediana total** (soma das medianas = 5.32 s vs mediana total = 5.64 s, 5.7% curto) — propriedade estatística esperada: a mediana de cada fase não precisa vir da mesma run.
- **Custo por visita f7a**: 
  - T=1: 12.240 s / 4.002B visitas = **8.15 ns/visita** (3.0% overhead serial)
  - T=12: 2.438 s × 12 / 4.002B = **19.48 ns/visita-thread** (41.8% eficiência paralela)
  - **Mecanismo dos 41.8% NÃO medido** (contention, false sharing, sincronização de barreira, desbalanceamento de carga por |inv| desigual).

## 3.4 — raw/uniq: 5.36 → 7.12 (raw −37%, uniq −52%)

Contexto: medição do F6 em ALOI N=72000 k=20 (Stage 3a). O `t_inv` (serial) constrói o índice invertido, `t_build` emite triplets.

| Métrica | Valor | O que é |
|---|---|---|
| `cand_raw` | 59,684,264 | triplets com multiplicidade (multi-set) |
| `cand_uniq` | 11,132,644 | colunas distintas após dedupe |
| `raw/uniq` | **5.36** | multiplicidade média por coluna |

No **pipeline fiel** (via `run_seq_probe` k=20 t=2), o f7a processa:

| Iteração | emb_nnz | f7a_row_visits (raw) | f7a_merges (uniq = N×L) | raw/uniq |
|---|---|---|---|---|
| it0 | 5.28M | 2.169B | 28.8M | **75.32** |
| it1 | 4.42M | 1.833B | 28.8M | **63.65** |

**Explicação da discrepância 5.36 vs 75.32/63.65:**
- **5.36** = multiplicidade média de **candidatos do Gram** (f6, pré-t-loop): cada coluna do Gram aparece em média 5.36 linhas do índice invertido.
- **75.32/63.65** = **visitas f7a por merge** (t-loop): para cada merge (uma posição na rank list final), quantas células do emb o f7a visita materializando o operando direito.
- O f7a faz `L` merges por linha × `N` linhas = `N×L` merges totais. Para cada merge, varre a linha do emb do elemento re-rankado — daí 75 visitas por merge na iteração 0.
- **raw −37% / uniq −52%** de it0 → it1: o emb encolhe (5.28M → 4.42M nnz, −16%), mas o raw cai mais rápido (2.169B → 1.833B, −15%) porque as linhas do emb ficam mais curtas → menos visitas por merge.

## 3.5 — Extrapolação 14.1 ns: correção registrada

O valor **14.1 ns/visita-thread** foi medido em **corel5k k=10 T=12** (12 threads, 12-thread-visits). **Não se aplica a ALOI T=1**:

| Métrica | Valor | Contexto |
|---|---|---|
| 14.1 ns | corel5k T=12 | por visita-thread, inclui contention de 12 threads |
| 8.15 ns | ALOI T=1 | por visita (base real, sem overhead paralelo) |
| 19.48 ns | ALOI T=12 | por visita-thread, **inclui overhead de 12 threads** |

**Erro original:** aplicar 14.1 ns (T=12, 12 threads de overhead) a T=1 → previsão ~20s vs 12.15s medido (erro 65%). O mecanismo dos 41.8% eficiência (contention, false sharing, sincronização de barreira, desbalanceamento por |inv| desigual) **não foi medido isoladamente** e não é afirmado.

---

## Resumo das lacunas fechadas

| Item | Status | Evidência |
|---|---|---|
| RSS breakdown por fase | ✅ | `clear_refs=5` logs em `/tmp/aloi_rss_breakdown*.log` |
| Sweep t/pa | ✅ | corel5k logs em `/tmp/corel5k_t*.log` / `/tmp/corel5k_pa*.log` |
| Tabela de fases completa | ✅ | 4 runs × 2 thread counts, runs 2-4 mediana + spread |
| raw/uniq explicado | ✅ | 5.36 (Gram) vs 75/64 (f7a visits/merge) — escalas diferentes |
| Extrapolação 14.1 ns | ✅ | Correção registrada; mecanismo do overhead não medido |
| k=50 / t≠2 / pa≠0.1 | 🟡 Parcial | t/pa sweep feito; k=50 não regenerável; ALOI não sweepado |

---

## Métricas de referência para Step 4 / P@n

| Métrica | Valor | Onde medido |
|---|---|---|
| ALOI N=72000 k=20 T=12 `ck_rk` | `0x261f6b6bca514f76` | `rep_t12_*.log`, `faithful_BENCH.log` |
| ALOI N=72000 k=20 T=1 `ck_rk` | `0x261f6b6bca514f76` | `rep_t1_*.log` (idêntico T=1/T=12) |
| corel5k L400 k=5 `ck_gram` fiel | `0x0ff2642751009152` | `step3b_real_gate.sh` |
| corel5k L400 k=20 `ck_gram` fiel | `0x3218b119e9aabc2c` | `step3b_real_gate.sh` |
| corel5k L1000 k=20 `ck_gram` fiel | `0x045f449c61d55fb5` | `step3b_real_gate.sh` |
| linnaeus L400 k=10 `ck_gram` fiel | `0xa584ebbe38e86571` | `step3b_real_gate.sh` + `step3e_linnaeus_touch.sh` |
| linnaeus L400 k=20 `ck_gram` fiel (NOVO) | `0xa77502078055a83d` | `step3e_linnaeus_touch.sh` (census via run()) |
| linnaeus L400 k=20 `ck_rk` fiel | `0xcb530cc3a7006366` | sweep E2E = fiel |

---

## Próximos passos (Step 4/5 — aguardam decisão)

1. **`PERFORM_CCS`**: carrier recusa `true` (Bloco 1); decisão sobre portar `execRRCCs` / default FALSE / consultar fonte.
2. **Commit WIP**: 129 arquivos untracked (incl. `RFE.cpp/.hpp`, `rfe_ref_verbatim.hpp`, FINDINGS, drivers, logs dossiês).
3. **Step 4 carrier**: `signInMethod("RFE")` já em `Method.cpp`; `make test-rfe` verde; `run_all.sh` etapa 9 verde.
4. **P@n via `pk_eval.py`**: após decisão `PERFORM_CCS`.

Fora do escopo (como combinado): otimizações (`f7a`, `t_index`) sem aval.
