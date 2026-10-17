# CPRR serial baseline profiling

This is a measurement harness, not a final K-d performance result. It profiles the
accepted serial K1-K6 candidate alongside the frozen `CprrSparseSerialOracle`
and fails unless their **final ranking ID arrays are exactly equal**.

## Reproduction

From the project root:

```sh
bash ports/CPRR/run_benchmark.sh \
  --n 5000 --L 400 --k 20 --t 2 \
  --input UDL_sandbox/fixtures/real/corel5k/rk_L400.txt \
  --seed 1 --commit "$(git rev-parse --short HEAD)" \
  --output /tmp/cprr_corel5k_profile.csv
```

For isolated RSS of one implementation, replace the default `--engine both`
with `--engine candidate` or `--engine oracle`; for a correctness gate, use
`--engine both` (and optionally `--mutation-probe`).

Set `CPRR_OPENMP=1` to compile the row-parallel candidate. In that mode K2,
O3-K3 destination gathers, and K6 use independent row iterations; K1 and K4
remain serial. Use `OMP_WAIT_POLICY=PASSIVE` for the host benchmark matrix.

For reproducible three-repeat timing comparisons, collect the engines in separate
processes with matching parameters and `--repeats 3`, using distinct CSV paths;
that also makes process HWM attributable to one engine. Runtime and RSS results
should include the exact compiler flags and host in any comparison report.

The profiler supports deterministic synthetic `random` and `hub` distributions,
or a text input with exactly `n*L` unsigned integer IDs. `--engine candidate`
profiles the candidate without retaining oracle state; `--engine oracle` measures
the frozen reference alone; `--engine both` performs the differential gate.
`--mutation-probe`
swaps a pair in a disposable final-list copy and verifies the exact-list check
rejects it. No production method settings or global compiler flags are changed.

## Measurements and caveats

- Per-kernel and per-engine wall times for both engines.
- `max_nref`, `sum_nref_squared` (K4 pair-work indicator), maximum absolute
  accumulator, live state-vector capacity bytes, and the maximum phase-boundary
  state-vector capacity observed during an engine run. Short-lived kernel scratch
  vectors are not included in this capacity metric.
- Process high-water RSS sampled after the requested run(s) using `getrusage`
  (Linux reports KiB). It is process-wide; in `--engine both` mode it includes
  both sequential runs and is not a per-engine or per-phase comparison. Use
  `--engine candidate` or `--engine oracle` for a single-engine RSS reading.
- Reference-host Corel5k, `n=5000,L=400,k=20,t=2`: final O1 candidate-only ran
  in 1.230 s wall and peaked at 36,224 KiB HWM; frozen oracle-only ran in
  1.235 s and peaked at 43,904 KiB HWM. Each is one isolated repeat, so timing
  is preliminary. Per-stage candidate timings and state-vector capacities are
  in `profile_corel5k_O1_candidate.csv`; oracle data are in
  `profile_corel5k_serial_oracle.csv`.
- O2 experiment: one candidate-only baseline measured 1.218 s wall time; median
  wall time over three stable-sort candidate runs was 1.259 s on the same Corel5k
  workload; O2 was reverted. This is preliminary single-process evidence.
- Metadata includes compiler version, parameters, seed/distribution, input path,
  commit hint, actual OpenMP thread count, and a requested `expected_threads`
  field for benchmark-matrix bookkeeping. Without `CPRR_OPENMP=1`, the harness
  reports one actual thread even when the expected count is set.

The random graph produces unique IDs per row with self included. The hub graph
adds the final ID to each non-hub row before filling other members. Input
validation rejects malformed IDs, duplicate row entries, missing self, and
invalid dimensions. Workloads that exceed available RAM should not be run on
the current reference host (~7.5 GiB available at inspection).

## Scope and staged roadmap

The default runner is serial and establishes the baseline for the K-d stages.
With `CPRR_OPENMP=1`, the safe row-parallel candidate measures 2/6/12-thread
throughput. K1 remains serial: source-row work mirrors writes into other rows,
which can race.

O1 (transfer the wrapper-owned input ranking into the candidate state) is staged
in the candidate. O2 (stable `O(L log L)` sorting) was tested on the Corel5k
`L=400` fixture and reverted: median candidate wall time increased from 1.218 s
to 1.259 s, with slower K2/K6 rows. These before/after numbers are preliminary
three-repeat measurements, not a universal conclusion.

O3 was then implemented for **K3 only**, as a serial destination-row gather.
The destination gathers source rows in increasing source-rank order by merging
its own source row into the ordered reverse bucket; this replays contributions
in the same order as the original serial scatter for each destination cell.
All eight integration fixtures passed the exact-final-ranking gate in strict
and fast-math, and passed ASan/UBSan. Additional tests passed ten deterministic
random seeds (`n=64,L=16,k=10,t=3`), hub distribution, ties, and dense `k=L`.
On the Corel5k sample, isolated K3 medians were 0.1494 s for the candidate and
0.1537 s for the frozen insertion-order oracle over three runs; O3's purpose is
race-free destination ownership for future row parallelism, not a demonstrated
serial throughput win. K4 remains the current serial reverse-cartesian kernel.

O4 then enabled OpenMP for the independent K2 rows, O3-K3 destination rows,
and K6 rows. K1 and K4 remain serial. Corel5k candidate-only wall times were
1.184 s / 0.869 s / 0.679 s / 0.647 s at 1/2/6/12 threads, respectively;
speedups versus one thread were 1.00× / 1.36× / 1.74× / 1.83×. The exact-list
differential matrix passed at all four thread counts; see
`profile_corel5k_openmp_matrix.csv` and
`profile_corel5k_openmp_differential.csv`. These are one-run measurements with
`OMP_WAIT_POLICY=PASSIVE`, not a claim that SMT always improves throughput.
OpenMP ASan/UBSan passed hub and ties. TSan was attempted but the host runtime
aborted before execution with `unexpected memory mapping`.

The agreed large candidate case (`n=72,000`, `L=400`, `k=100`, `t=3`, seed
1729, random) was attempted but stopped after about 3 minutes at ~566 MiB RSS
and ~96% of one CPU core; no completed CSV was produced. This is not an OOM or a
completed scale result. Do not repeat this high-workload case until O4 or another
measured algorithmic reduction changes the work profile. O3 is now the serial
destination gather and O4 is the safe row-parallel stage; next consider O5 hybrid
K4. O6 (removing global values) is deferred pending memory/bandwidth evidence.