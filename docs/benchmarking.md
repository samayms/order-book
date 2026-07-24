# Benchmark Baseline

## Method

The benchmark is a dependency-free executable so it can run anywhere the library can
build. It pre-generates a deterministic seven-operation cycle:

1. add two resting asks;
2. add one resting bid;
3. submit a market buy;
4. submit a crossing IOC buy;
5. cancel the remaining ask;
6. cancel the resting bid.

Each cycle returns the book to empty. Before measuring, the harness prefills a
realistic resting book — by default 10,000 GTC sell orders spread across 5,000
price levels far above the workload's top ask, so they are never matched or
cancelled but make every price-level map operation pay a realistic `O(log P)`
cost. The throughput pass replays the same request shape without per-operation
clock calls. A separate latency pass times individual mixed operations. Both use
`NullEventSink`, and every pass ends with invariant validation. The checksum is
printed to make unintended work elimination visible.

The direct-core benchmark deliberately excludes the mutex, future allocation, and
thread scheduling of `OrderBookEngine`; those are coordination costs and need a
separate producer-to-consumer benchmark.

## Reproduce

```sh
cmake --preset release
cmake --build --preset release
./build/release/orderbook_benchmark --operations 350000 --iterations 200
```

Latency is reported separately for limit-GTC, limit-IOC, market, and cancel
operations and includes clock-call overhead. Use it to compare identical
builds/workloads, not as an exchange-wire latency claim.

## Historical ledger (superseded workload — do not compare directly)

Measured on 2026-07-22 with Apple Clang 17.0.0, arm64, macOS 26.5, 70,000,000
throughput operations after warmup. **These numbers predate the 10,000-order depth
prefill described in Method above** — they ran against an essentially empty book, so
the price-level maps held ~1 level and every map operation was near-O(1). They are
**not comparable** to the current results below and are retained only as a record of
that earlier, shallower workload.

| Variant | Hypothesis | Throughput | p50 | p95 | p99 | Correct? |
|---|---|---:|---:|---:|---:|---|
| baseline run 1 | documented map/pool/index design, no depth prefill | 53,819,548 ops/s | 41 ns | 42 ns | 42 ns | yes |
| baseline run 2 | repeatability check | 53,437,158 ops/s | 41 ns | 42 ns | 42 ns | yes |
| baseline run 3 | repeatability check | 53,646,229 ops/s | 41 ns | 42 ns | 42 ns | yes |

## Current ledger (2026-07-23, 10,000-order depth prefill)

Environment: Apple M4-class arm64, Apple Clang 17.0.0, release build with `-O2`
and CMake IPO/LTO enabled. Workload: `--operations 350000 --iterations 200` =
70,000,000 ops over a 10,000-order / 5,000-level resting book. Every run reported
checksum `3499512000` and zero rejections; the debug build's invariant validator
passes after each pass.

Progression of the release configuration:

| Stage | Throughput (median) | Notes |
|---|---:|---|
| tombstone-divisor tuning (divisor 8) | ~21.9M ops/s | earlier threshold-sweep loop |
| + LTO/IPO, then `-O2` over `-O3` | ~22.7M ops/s | build-tuning loop |
| + hot-path structural wins (this session) | **~31.9M ops/s** | **+40% over the 22.7M baseline** |

Hot-path optimization results (each change isolated vs the 22.7M baseline; strict
10-round interleaved A/B with a paired t-test, 95% threshold |t|>2.26 at n=10):

| Change | median Δ | rounds won | paired t | significant |
|---|---:|:---:|---:|---|
| PMR price-level node pooling | +17.6% | 10/10 | +31.16 | yes |
| Top-of-book cancel fast path | +8.0% | 10/10 | +7.27 | yes |
| Non-crossing GTC fast path | +6.5% | 10/10 | +7.46 | yes |
| Two-phase GTC index insert (`reserve`/`commit`) | +1.3% | 9/10 | +2.74 | yes (marginal) |
| **All four combined** | **+40%** | — | — | — |

Latency improved alongside throughput: limit-GTC p95 83→42 ns and p99 125→84 ns;
cancel p95/p99 84→42 ns. The four effects compound (≈ 1.083 × 1.173 × 1.010 × 1.055
≈ 1.42) rather than overlapping.

The combined change was also evaluated with a temporary separate holdout harness:
deep same-price FIFO sweep +10%, multi-price-level sweep +47%, and randomized
adds/cancels +10%, with no observed regression. These supplemental measurements
are recorded as historical evidence; the checked-in benchmark command above is
the reproducible performance gate.

Maximum latency is intentionally omitted from comparisons because it is dominated by
OS scheduling noise in this short local run; the executable still prints it.

## Multibook coordination benchmark (FourBookEngine)

`benchmarks/four_book_benchmark.cpp` measures the **coordination layer** of
`FourBookEngine` — routing, four per-book mutex/condition-variable queues, four
worker threads, and the `std::future` completion path. It **must not** be compared
with the single-book direct-core benchmark above; they measure different layers,
and the coordination numbers include per-request future overhead.

The workload is deliberately order-independent: every add is a non-crossing buy and
every cancel targets one of the same producer's own still-resting orders. Under any
thread interleaving the result is zero rejections and a deterministic per-book state
and checksum, so a nonzero rejection count or a changed checksum flags a real
defect. Each scenario runs a warmup, a throughput pass, and a bounded-in-flight
(32 requests/producer) latency pass, and validates all four books after shutdown.

Scenarios: `single` (1 producer → 1 book, the one-worker ceiling), `independent`
(4 producers, one book each, no queue contention), `balanced` (4 producers spread
evenly over all books), and `skewed` (~80% to one hot book).

```sh
cmake --preset release
cmake --build --preset release
./build/release/four_book_benchmark
```

Illustrative run (2026-07-23, Apple M4-class arm64, 12 logical cores, `-O2` + LTO;
all scenarios reported zero rejections and valid books). Numbers vary run to run;
this is a qualitative snapshot, not a gate:

| Scenario | Aggregate throughput | Notes |
|---|---:|---|
| single | ~3.0M ops/s | one book, one worker — coordination ceiling |
| independent | ~7.3M ops/s | ~2.4× single; four books scale with no cross-book contention |
| balanced | ~3.1M ops/s | even per book, limited by four producers contending on all queues |
| skewed | ~2.2M ops/s | bottlenecked on the hot book while the others idle |

Enqueue-to-completion latency at the 32-request in-flight window was ~12 µs p50 for
`single` and rose with contention (tens of µs). The gap from linear 4× scaling is
the producer/completion path (promise/future per request, queue-mutex contention),
which is where Phase 3 would evaluate a bounded response channel or batched
completions against the current future API. Four workers help aggregate throughput
only when independent books keep them busy and the completion path is not the
bottleneck — the benchmark, not the thread count, decides whether it helps a given
workload.

## Optimization protocol

The baseline is not a claim of optimality. For a tuning session:

- keep the test and invariant gates green;
- use the same compiler, flags, workload, and iteration count;
- change one data-layout/container hypothesis per variant;
- run baseline and candidate at least three times;
- reject candidates inside observed noise or with worse tail latency;
- limit an iteration to five variants or thirty minutes before reassessing evidence;
- promote only the best measured safe variant with a simple rollback diff.

Candidate experiments include a flat price-level container, alternative ID-index
probing, and batched event delivery. They are deferred until profiling identifies a
bottleneck.
