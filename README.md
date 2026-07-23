# C++ Order Book

A deterministic, in-memory C++20 limit-order-book and matching engine rebuilt from
the original single-file prototype in [`main.cpp`](main.cpp). It is designed as a
clear, tested, benchmarkable systems project—not as a claim of exchange production
readiness.

## Highlights

- price-time priority with executions at the resting maker's price;
- limit GTC, limit IOC, limit FOK, market IOC, and cancellation by ID;
- strong numeric order IDs and checked four-decimal fixed-point prices;
- preallocated order-node arena with stable handles and intrusive FIFO lists;
- fixed-capacity open-addressed ID index with no per-order insertion allocation;
- immediate O(1) unlink after ID/level lookup and correct level aggregates;
- typed request results plus bounded POD execution/rejection sinks;
- lockless-by-ownership `OrderBook` and optional single-worker `OrderBookEngine`;
- debug invariant validation, deterministic stress coverage, sanitizers, and a
  reproducible throughput/latency baseline.

The full behavioral contract, ownership model, complexity, invariants, and design
tradeoffs live in [docs/architecture.md](docs/architecture.md). Benchmark method and
measured baseline live in [docs/benchmarking.md](docs/benchmarking.md).

## Architecture

```text
producer threads
      |
      v
OrderBookEngine ---- mutex/CV FIFO, one owned worker (optional)
      |
      v
OrderBook ---------- synchronous single-writer matching core
  |       |       |
  |       |       +-- POD executions/rejections -> EventSink
  |       +---------- fixed-capacity OrderId -> OrderHandle index
  +------------------ preallocated order pool + ordered price levels
```

The core performs no console I/O and takes no locks. Order nodes and ID-index storage
are allocated during construction. The `std::map` price-level containers draw their
nodes from a `std::pmr::unsynchronized_pool_resource`, so a price level emptied and
later recreated recycles its node; a fresh allocation happens only when the peak
level count grows. The benchmark includes that cost.

## Build and run

Requirements: a C++20 compiler, CMake 3.20+, and POSIX threads.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release

./build/release/orderbook_demo
./build/release/orderbook_benchmark
```

The release configuration enables link-time / interprocedural optimization
(`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`, guarded by a CMake support check)
and compiles at `-O2` rather than CMake's default `-O3`. LTO is the large,
robust win: on this host it improves benchmark throughput ~4-5% in a 10-round
paired study (significant, t=-3.87). `-O2` is a smaller, workload-specific edge
over `-O3` (~1-1.6% in paired runs) — `-O3`'s heavier inlining/unrolling costs
more than it buys on this branchy `std::map` + hash-probe code. Both hold
correctness identical (same checksum, zero rejections, invariants intact). Debug
and sanitizer presets deliberately omit LTO and keep the default `-O0`/`-O2`.

Debug and sanitizer presets:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

Verified on this development host (Apple M4 Pro, Apple Clang 17, CMake 4.4,
2026-07): strict debug and release builds plus CTest pass, and the full suite and
benchmark also pass under UBSan built with
`-fsanitize=undefined -fno-sanitize-recover=undefined`. The installed Apple ASan
runtime deadlocks before `main` even for a hello-world program (re-entrant
`AsanInitInternal` -> `InitializeShadowMemory` -> dyld shared-cache query ->
`malloc` -> ASan init spinlock), and TSan exits 139 for the same hello-world;
those two runtime checks run in CI instead: GitHub Actions builds and tests the
debug, release, asan-ubsan, and tsan presets under both gcc and clang on
ubuntu-latest (all passing as of 2026-07-23), with a release-mode benchmark
validation run.

## Tests

The current suite has 19 deterministic cases across two executables. Coverage includes
both matching directions, maker-price improvement, FIFO and multi-level sweeps,
partial/full fills, GTC/IOC/FOK, market remainder, every cancellation position,
invalid/duplicate requests, capacity reuse, event bounds, a 5,000-operation invariant
stress sequence, queue draining, idempotent shutdown, and concurrent producers.

## Repository layout

```text
include/orderbook/   public C++20 headers
src/                 matching engine implementation
tests/               core and threaded-engine tests
apps/                runnable demo
benchmarks/          deterministic benchmark executable
docs/                architecture and benchmark methodology
main.cpp, test.cpp   original prototype retained as historical input
```

The root prototype files are excluded from build targets so the evolution from the
initial design remains reviewable.
