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

Each cycle returns the book to empty. The throughput pass replays the same request
shape without per-operation clock calls. A separate latency pass times individual
mixed operations. Both use `NullEventSink`, and every pass ends with invariant
validation. The checksum is printed to make unintended work elimination visible.

The direct-core benchmark deliberately excludes the mutex, future allocation, and
thread scheduling of `OrderBookEngine`; those are coordination costs and need a
separate producer-to-consumer benchmark.

## Reproduce

```sh
cmake --preset release
cmake --build --preset release
./build/release/orderbook_benchmark --operations 350000 --iterations 200
```

The latency distribution combines request types and includes clock-call overhead.
Use it to compare identical builds/workloads, not as an exchange-wire latency claim.

## Baseline ledger

Measured on 2026-07-22 with Apple Clang 17.0.0, arm64, macOS 26.5. Each run processed
70,000,000 throughput operations after warmup.

| Variant | Hypothesis | Throughput | p50 | p95 | p99 | Correct? |
|---|---|---:|---:|---:|---:|---|
| baseline run 1 | documented map/pool/index design | 53,819,548 ops/s | 41 ns | 42 ns | 42 ns | yes |
| baseline run 2 | repeatability check | 53,437,158 ops/s | 41 ns | 42 ns | 42 ns | yes |
| baseline run 3 | repeatability check | 53,646,229 ops/s | 41 ns | 42 ns | 42 ns | yes |

The three-run throughput spread is below 1%. Maximum latency is intentionally omitted
from comparisons because it is dominated by OS scheduling noise in this short local
run; the executable still prints it.

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
