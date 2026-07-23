# Order Book Architecture

## 1. Purpose and scope

This document is the technical source of truth for the rebuild of the original
single-file order book in `main.cpp`. The rebuild preserves its useful behavior—
limit orders, market orders, cancellation, price improvement, and price-time
priority—while replacing unsafe or unmeasurable implementation choices.

Version 1 covers one instrument per `OrderBook` instance. It does not implement a
network protocol, persistence/recovery, risk checks, authentication, multi-symbol
routing, or exchange-specific auction rules. Those belong around the matching core.

## 2. Behavioral contract

### Supported requests

- **Limit/GTC:** match immediately; rest any remainder at the limit price.
- **Limit/IOC:** match immediately; cancel any remainder.
- **Limit/FOK:** execute the entire quantity immediately or reject without mutation.
- **Market/IOC:** consume available opposite-side liquidity at any price; never rest.
- **Cancel:** remove an active resting order by ID in constant time after lookup.

Order IDs are unique among active orders. Reuse is allowed after an order is fully
filled or cancelled. A replace operation is deliberately deferred because priority
retention rules must be specified before it can be implemented correctly.

### Matching rules

1. Select the best opposite price: lowest ask for a buy, highest bid for a sell.
2. For a limit order, stop if that price no longer crosses the limit.
3. Match the oldest resting order at the level.
4. Execute `min(taker_remaining, maker_remaining)` at the maker's price.
5. Remove a fully filled maker immediately; erase an empty price level immediately.
6. Continue until the taker is filled, its price no longer crosses, or liquidity ends.

The same request stream must always produce the same executions and final state.

## 3. Domain types and API boundary

The core avoids ambiguous primitive inputs:

| Type | Representation | Rule |
|---|---:|---|
| `OrderId` | `uint64_t` wrapper | `0` is invalid |
| `Price` | signed 64-bit tick wrapper | must be greater than zero |
| `Quantity` | `uint32_t` | must be greater than zero |
| `AggregateQuantity` | `uint64_t` | per-level sum |
| `SequenceNumber` | `uint64_t` | monotonically increasing execution sequence |
| `OrderHandle` | `uint32_t` | stable index into the order pool |

Decimal/string conversion is an adapter concern. The provided conversion helper uses
four decimal places and checked rounding, but matching APIs accept `Price` directly.

Expected failures use `Status` and `SubmitResult`. Invalid construction and unexpected
allocator failure may throw. For GTC, the potential `std::map` price-level allocation
is prepared before matching and fixed resources are cleaned up if it fails, so an
allocation exception cannot follow partial execution. Rejections never partially
mutate the book. FOK performs a read-only liquidity preflight before matching.

## 4. Component model

```text
producer threads
      |
      v
OrderBookEngine -------- mutex/CV FIFO + futures (coordination layer)
      |
      | exactly one worker mutates
      v
OrderBook -------------- single-writer matching core, no locks
  |       |       |
  |       |       +---- EventSink -> execution/rejection POD events
  |       +------------ OrderIndex -> fixed-capacity open-addressed ID table
  +-------------------- OrderPool + bid/ask price-level maps
```

### `OrderBook`

Owns all matching state and exposes synchronous request methods and read-only views.
It is intentionally not thread-safe. This keeps its hot path free of synchronization
and makes ordering deterministic.

### `OrderBookEngine`

Is an optional coordination layer. Multiple producers may enqueue commands; one
owned worker processes them in queue order. Shutdown stops admission, drains accepted
commands, joins exactly once, and rejects subsequent submissions. The engine never
creates multiple consumers for one book.

### `EventSink`

Receives non-owning, numeric POD events synchronously. Callbacks are `noexcept` and
must not call back into the book. `NullEventSink` supports benchmarks;
`RecordingEventSink` uses caller-selected preallocated capacity for tests; a printing
adapter performs I/O outside the core.

## 5. Core data structures

### Order pool

A `std::vector<OrderNode>` is sized once in the constructor. Handles are vector
indices and remain stable. Free nodes form an intrusive freelist. Active nodes also
carry intrusive `previous` and `next` handles for their price-level FIFO.

- allocate/free: O(1)
- stable references for the book lifetime
- no shared ownership or per-order-node allocation
- hard, explicit active-order capacity

### Order ID index

A fixed-capacity, open-addressed hash table maps `OrderId` to `OrderHandle`. It is
allocated once at construction, uses empty/occupied/tombstone states, and maintains a
load factor below 0.5. This removes the per-entry allocations of
`std::unordered_map<std::string, ...>` from the prototype and partial rewrite.

- expected lookup/insert/erase: O(1)
- worst case: O(table capacity)
- no allocation after construction

### Price levels

Bids use `std::map<Price, Level, std::greater<>>`; asks use ascending `std::map`.
The best price is always `begin()`. A `Level` stores FIFO head/tail handles, order
count, and aggregate remaining quantity. The maps draw their nodes from a
`std::pmr::unsynchronized_pool_resource` owned by the book, so a level emptied and
later recreated recycles its node instead of round-tripping the global allocator.

- best price: O(1)
- create/find/erase price level: O(log P), where P is active price levels
- append/unlink within level: O(1)
- creating a level allocates a map node only when the pool has no recycled node to
  hand back (i.e. when the peak level count grows); under steady churn the node is
  reused, so this is the only intentional matching-state allocation after
  construction and it is amortised away once the working set is warm

Specialized flat or bounded price structures are future benchmark variants, not
assumed improvements.

### Implemented hot-path optimizations

Measured wins layered on the baseline design, each proven on the benchmark plus
deep-FIFO, multi-level, and randomized holdout workloads with identical checksums:

- **PMR level-node pooling** (above): recycles map nodes across level churn.
- **Top-of-book cancel fast path**: when the cancelled order's price is already
  `begin()`, its level iterator is taken in O(1) instead of an O(log P) find.
- **Two-phase GTC index insert** (`OrderIndex::reserve`/`commit`): one probe both
  detects a duplicate and locates the destination slot; the slot is committed in
  O(1) only if a remainder rests, replacing a separate `contains`+`insert`.
  Duplicate detection stays before the pool allocation, preserving
  duplicate-over-capacity precedence and rejection atomicity.
- **Non-crossing GTC fast path**: a GTC that cannot cross the best opposite price
  rests directly without entering the matching loop.

## 6. Request lifecycle and atomicity

### Limit/GTC

Validate ID, price, quantity, and duplicate status. Reserve an order-pool slot before
mutation. Match against the opposite side. Release the slot if fully executed;
otherwise append the remainder to its price level and index it. If capacity is full,
the request is rejected before any execution.

### Limit/IOC and market

Validate first, then match from a stack-local taker representation. No pool slot is
needed because the remainder cannot rest.

### Limit/FOK

Sum crossing level aggregates without mutation. Reject with `would_not_fully_fill`
if total executable liquidity is insufficient; otherwise execute through the normal
matching path. Because the preflight guarantees no remainder, no pool slot is needed.

### Cancel

Look up the handle, unlink it from the doubly linked FIFO, decrement level aggregates,
erase an empty level, erase the ID index entry, and return the node to the freelist.

## 7. Invariants

The debug validator and tests enforce:

- no crossed book remains after any public operation;
- all resting quantities and prices are positive;
- every active node appears exactly once in one level and once in the ID index;
- pool active count, index size, and traversed node count are equal;
- level head/tail and previous/next links are bidirectionally consistent;
- level order counts and aggregate quantities equal traversal results;
- node side and price agree with the containing level;
- best bid and ask are the first entries of their respective maps;
- empty levels are absent;
- execution sequence numbers are strictly increasing;
- rejected requests leave book state unchanged.

`validate()` is an O(capacity + active orders + price levels) diagnostic and is not
called on the production hot path.

## 8. Complexity summary

| Operation | Expected complexity |
|---|---|
| Add non-crossing limit | O(1) index + O(log P) level |
| Match | O(executions + emptied levels × log P) |
| Cancel by ID | O(1) expected index + O(log P) level lookup |
| Best bid/ask | O(1) |
| FOK preflight | O(crossing price levels) |
| Invariant validation | O(pool capacity + active orders + P) |

## 9. Concurrency and ownership

`OrderBook` follows single-writer ownership, not lock-free concurrency. Callers either
use it synchronously from one thread or submit through `OrderBookEngine`. Event sinks
outlive the book/engine that observes them. The worker never holds its queue mutex
while matching or invoking the sink. Futures are control-plane conveniences and are
not included in core matching benchmarks.

## 10. Repository organization

```text
include/orderbook/
  types.hpp          strong domain types, statuses, requests/results
  events.hpp         POD events and sinks
  order_pool.hpp     fixed-capacity node arena and freelist
  order_index.hpp    fixed-capacity ID-to-handle hash index
  order_book.hpp     synchronous matching API and views
  engine.hpp         optional serialized producer interface
src/
  events.cpp
  order_pool.cpp
  order_index.cpp
  order_book.cpp
  engine.cpp
tests/
  test_order_book.cpp
  test_engine.cpp
apps/
  demo_main.cpp
benchmarks/
  orderbook_benchmark.cpp
docs/
  architecture.md
```

Root `main.cpp` and `test.cpp` remain as historical prototype inputs during the
rebuild and are excluded from CMake targets.

## 11. Verification and performance gates

Correctness tests cover conversions, both matching directions, price improvement,
FIFO, multi-level sweeps, all cancellation positions, partial/full fills, GTC/IOC/FOK,
market remainder, invalid/duplicate requests, capacity/reuse, top-of-book aggregates,
invariants, queue draining, shutdown, and concurrent producers.

Build gates use strict warnings, CTest, ASan/UBSan, and TSan where supported. Tests
have no network or real-time sleeps.

The benchmark has two separate passes:

1. throughput without per-operation clock calls;
2. sampled/direct latency with p50/p95/p99/max.

It uses deterministic pre-generated requests, a warmup, a null sink, release mode,
and final invariant validation. The initial version establishes a baseline only.
Optimization variants must change one hypothesis at a time and beat repeated baseline
measurements without failing correctness.

## 12. Lessons from the prototypes

The original `main.cpp` established semantics but used floating-point hash keys,
`shared_ptr` order ownership, duplicate heap/level state, lazy cancellation, global
queue state, I/O during matching, and unsafe two-consumer shutdown.

The first partial rewrite improved ticks, pooling, intrusive lists, and events, but it
still allocated strings and hash nodes on active-order insertion, constructed temporary
strings for lookup, declared allocating recording callbacks `noexcept`, exposed weak
primitive aliases, omitted time-in-force, and did not make shutdown safe against
repeated calls. The rebuild retains the useful concepts while correcting those edges.

## 13. Deferred extensions

- cancel/replace semantics and priority rules;
- multi-instrument router and per-symbol partitioning;
- binary market-data/order-entry adapters;
- persistence, snapshots, and replay recovery;
- bounded/faster price-level containers selected by benchmark evidence;
- property-based fuzzing against a simple reference book;
- hardware-counter and allocation instrumentation.

## 14. Design decision record

A single index of the significant choices and their rationale. Measured decisions
link to [`benchmarking.md`](benchmarking.md) for the numbers rather than repeating
them here; the sections above give the mechanics.

### Domain and API

- **Integer tick prices, numeric order IDs, fixed-width quantities in the core**
  (§3). Rationale: exact, hashable, deterministic ordering with no floating-point
  rounding in matching. Decimal/string forms are an adapter concern.
  `price_from_double` rejects NaN/±inf and non-positive input; this correctness
  guarantee is why `-ffast-math` is disallowed (its finite-math assumptions can
  invalidate those checks).
- **Typed `Status`/`SubmitResult` for expected failures; exceptions only for
  invalid construction and unexpected allocator failure** (§3, §6). Rationale:
  request-level failures are ordinary control flow and must not unwind; a rejected
  request never partially mutates the book.

### Core data structures

- **Preallocated `std::vector<OrderNode>` order pool with an intrusive freelist;
  orders reference each other by 32-bit `OrderHandle` indices, not pointers**
  (§5 Order pool). Rationale: O(1) `noexcept` allocate/release, stable references,
  no per-order heap allocation or `shared_ptr` ownership, trivially serialisable
  links. Free nodes use the dedicated `next_free` handle; active nodes use
  `previous`/`next` for their price-level FIFO.
- **Fixed-capacity open-addressed hash index (`OrderId → OrderHandle`) with
  empty/occupied/tombstone slots and a power-of-two mask**, chosen over
  `std::unordered_map<std::string, …>` (§5 Order ID index). Rationale: O(1)
  expected lookup at load factor < 0.5 with zero per-entry allocation after
  construction.
- **Tombstone deletion with a preallocated scratch-table rebuild when tombstones exceed
  `table_size / 8`** (`ORDERBOOK_TOMBSTONE_REBUILD_DIVISOR`). Rationale: the
  divisor was swept as a compile-time knob; 8 was the measured peak (too-rare
  rebuilds lengthen probe chains, too-frequent rebuilds dominate). See
  `benchmarking.md`. Backward-shift deletion was considered and rejected for this
  iteration: relocating entries on erase is incompatible with the current
  slot-index reservation token, and profiling did not identify the index as the
  next bottleneck.
- **`std::map<Price, Level>` (descending for bids, ascending for asks); best price
  is always `begin()`** (§5 Price levels). Rationale: O(1) best price, ordered
  iteration for sweeps, O(log P) create/find/erase. Flat/bounded price structures
  remain a deferred, benchmark-gated variant, not an assumed win.

### Allocation and hot-path optimizations (measured; see `benchmarking.md`)

- **PMR node pooling: the price-level maps draw nodes from a
  `std::pmr::unsynchronized_pool_resource` owned by the book.** Rationale: levels
  emptied and recreated under churn recycle their tree node instead of
  round-tripping the global allocator; the largest single measured win.
- **Top-of-book cancel fast path:** when the cancelled order's price is already
  `begin()`, take the level iterator in O(1) instead of an O(log P) find.
- **Non-crossing GTC fast path:** a GTC that cannot cross the best opposite price
  rests directly without entering the matching loop.
- **Two-phase GTC index insert (`OrderIndex::reserve`/`commit`):** one probe both
  detects a duplicate and locates the destination slot; the slot is committed in
  O(1) only if a remainder rests. Preserves duplicate-over-capacity precedence
  (duplicate is detected before the pool allocation) and rejection atomicity.
  Each of these four was validated in isolation with interleaved A/B benchmarking
  and a paired t-test. A temporary supplemental holdout harness found no regression;
  the checked-in standard workload remains the reproducible performance gate.

### Concurrency

- **Single-writer, lock-free-by-ownership `OrderBook`; optional `OrderBookEngine`
  serialises many producers onto one worker** (§4, §9). Rationale: keeps the hot
  path free of synchronisation and makes ordering deterministic. This is *not*
  lock-free concurrency, and parallel mutation of one `OrderBook` is unsupported
  by design.
- **Event callbacks receive `noexcept` POD events; no console I/O in matching code**
  (§4 EventSink). Rationale: the matching core stays allocation- and I/O-free;
  printing/recording adapters live outside it.

### Build configuration

- **Release compiles at `-O2` with link-time optimization, enabled
  via `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` behind a `check_ipo_supported`
  guard; debug and sanitizer presets keep the default `-O0`/`-O2` and no LTO.**
  Rationale: LTO is a large, statistically significant throughput win; `-O2` beat
  the CMake-default `-O3` by a small but repeatable margin on this branchy
  map/hash-probe code (`-O3`'s heavier inlining/unrolling costs more than it buys).
  Both hold correctness identical (same checksum, zero rejections). See
  `benchmarking.md` for the measurements and the paired-comparison methodology.
