# C++ Order Book

A deterministic, in-memory C++20 limit-order-book and matching engine. It is
designed as a clear, tested, benchmarkable systems project—not as a claim of
exchange production readiness.

## Highlights

- price-time priority with executions at the resting maker's price;
- limit GTC, limit IOC, limit FOK, market IOC, and cancellation by ID;
- strong numeric order IDs and checked four-decimal fixed-point prices;
- preallocated order-node arena with stable handles and intrusive FIFO lists;
- fixed-capacity open-addressed ID index with no per-order insertion allocation;
- immediate O(1) unlink after ID/level lookup and correct level aggregates;
- typed request results plus bounded POD execution/rejection sinks;
- lockless-by-ownership `OrderBook` and optional single-worker `OrderBookEngine`;
- optional `FourBookEngine` routing facade — four independent books, four workers,
  parallel matching across instruments with no lock added to the matching path;
- merged book-tagged event stream over bounded lock-free SPSC rings, so one consumer
  can observe all four books without sharing a sink or blocking matching;
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

For matching several instruments in parallel, `FourBookEngine` routes by `BookId`
over four independent shards — no dispatcher thread and no lock on the matching path:

```text
producers --> FourBookEngine (route by BookId) --> [ 4x OrderBookEngine + OrderBook ]
                                                          |  worker i is sole producer
                                                          v
                     one consumer <-- MergedEventStream <-- 4x lock-free SPSC ring
```

Requests for different books run concurrently; requests for one book stay serialized.
Each book keeps its own worker, queue, and event sink; a `MergedEventStream` lets one
consumer observe all four via bounded lock-free SPSC rings (drop-and-count on overflow,
never blocking matching). Design, phases, and the coordination benchmark are in
[docs/four-book-threading-plan.md](docs/four-book-threading-plan.md) and
[docs/benchmarking.md](docs/benchmarking.md).

## Build and run

Requirements: a C++20 compiler, CMake 3.20+, and POSIX threads.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release

./build/release/orderbook_demo
./build/release/orderbook_benchmark

# four independent books matched in parallel by four worker threads:
./build/release/four_book_demo
./build/release/four_book_events_demo  # four books -> one merged, book-tagged feed
./build/release/four_book_benchmark    # coordination-layer benchmark (see docs)
```

`four_book_demo` scripts a small trade scenario on each of four books concurrently
and prints per-book results. `four_book_benchmark` measures the `FourBookEngine`
coordination layer (routing, four queues/workers, futures) across balanced, skewed,
independent, and single-book shapes; its method and a sample run are in
[docs/benchmarking.md](docs/benchmarking.md). It is not comparable to the
single-book `orderbook_benchmark` — they measure different layers.

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

The suite spans four deterministic executables — the matching core, the
single-worker `OrderBookEngine`, the four-book `FourBookEngine`, and the merged
event stream. Coverage includes both matching directions, maker-price improvement,
FIFO and multi-level sweeps, partial/full fills, GTC/IOC/FOK, market remainder,
every cancellation position, invalid/duplicate requests, capacity reuse, event
bounds, a 5,000-operation invariant stress sequence, queue draining, idempotent
shutdown, concurrent producers, per-book routing/isolation, independent sequence
numbers, shutdown-race accounting, and — for the event stream — SPSC ring FIFO,
concurrent merged delivery with correct book tags, and counted overflow drops.

## Repository layout

```text
include/orderbook/   public C++20 headers
src/                 matching engine implementation
tests/               core and threaded-engine tests
apps/                runnable demo
benchmarks/          deterministic benchmark executable
docs/                architecture and benchmark methodology
```
