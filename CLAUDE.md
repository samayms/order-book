# Order Book Engine — Contributor Instructions

`AGENTS.md` is a symlink to this file. Keep this file as the single source of
durable instructions shared by Codex and Claude.

## Source of truth

- Read [`docs/architecture.md`](docs/architecture.md) before changing the engine.
- Keep implementation, tests, this file, the architecture document, and
  `README.md` consistent.
- Treat test results and the implementation as current-state truth. Clearly mark
  future work; never document planned behavior as already implemented.

## Project goal

Build a deterministic, resume-quality, in-memory C++20 limit-order-book and
matching engine. Correctness and explicit semantics come before performance;
performance claims require reproducible measurements.

## Non-negotiable engine rules

- Price-time priority: best price first, FIFO within a price level.
- Trades execute at the resting maker's price.
- Core prices are integer ticks; core order IDs are numeric and quantities are
  fixed-width integers. Floating point and human-readable IDs belong in adapters.
- Expected request failures return typed status values. Invalid construction and
  unexpected allocator failure may throw; allocation failure while preparing a GTC
  price level occurs before matching and is cleaned up without partial execution.
- Cancelled and fully filled orders are unlinked immediately.
- `OrderBook` is single-writer and contains no locks.
- Callbacks receive POD events and must be `noexcept`; no console I/O occurs in
  matching code.
- No allocation of order nodes or ID-index entries after `OrderBook` construction.
  Creating a previously unseen `std::map` price level may allocate and must be
  described honestly in performance documentation.
- Public mutating operations must leave all documented invariants true before
  returning, including on rejection.

## Engineering conventions

- C++20, RAII, const-correctness, scoped enums, no raw `new`/`delete`, and no
  `using namespace std` in production code.
- Use strong domain types and avoid implicit narrowing conversions.
- Prefer dependency-free standard-library code unless a dependency provides clear,
  measured value.
- Compile production targets with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`;
  warnings are errors in project verification.
- Add or update tests before changing matching behavior. Tests must be deterministic
  and must call the debug invariant validator after mutation-heavy scenarios.
- Keep benchmarks separate from tests. Use deterministic workloads, a no-op event
  sink, release optimization, warmup, correctness checks, and reported percentiles.
- Do not optimize from intuition alone. Record a baseline, change one hypothesis,
  keep tests green, and repeat measurements before promoting a variant.

## Repository organization

```text
include/orderbook/   public library headers
src/                 library implementations
tests/               deterministic unit/integration tests
apps/                runnable examples and adapters
benchmarks/          reproducible performance harnesses
docs/                architecture and design decisions
main.cpp, test.cpp   original prototype retained as historical input only
```

The original root files are not build targets. Remove or move them only after the
new engine has behavior-parity tests and the user approves cleanup.

## Completion gate

1. Strict debug and release builds succeed with zero warnings.
2. All CTest tests pass.
3. ASan and UBSan pass; TSan passes for threaded-engine changes when supported by
   the installed toolchain.
4. The benchmark runs in release mode, validates the resulting book, and reports
   workload, operation count, throughput, and latency percentiles.
5. `README.md` contains commands that were actually verified in this environment,
   or clearly states any unavailable tool.
