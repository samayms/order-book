# Four-Book / Four-Worker Architecture Plan

> **Status:** Phases 1, 2, and the event-output half of Phase 3 are
> **implemented**. Phase 1 (correctness-first composition, §10) —
> `include/orderbook/four_book_engine.hpp` (+ `book_id.hpp`),
> `src/four_book_engine.cpp`, and `tests/test_four_book_engine.cpp` — provides
> `BookId`, `FourBookConfig`, and `FourBookEngine` (routing, per-book isolation,
> lifecycle/shutdown). Phase 2 (the multibook benchmark, §9) is
> `benchmarks/four_book_benchmark.cpp`; `apps/four_book_demo.cpp` is a runnable
> example. **Phase 3a — the book-tagged merged event stream** (§5) — is
> `include/orderbook/event_stream.hpp` (`SpscRing`, `RoutedEvent`,
> `RoutingEventSink`, `MergedEventStream`), with `tests/test_event_stream.cpp` and
> `apps/four_book_events_demo.cpp`: bounded lock-free single-producer/single-consumer
> rings per book, drop-and-count overflow policy, one consumer merging all four
> books. **Phase 3b — the measured completion-path change** (futures vs a bounded
> response channel or batched completions) and any CPU-affinity/busy-poll tuning —
> is **not yet implemented**; it is an evidence-gated optimization and remains a
> proposal below. TSan for the concurrent tests could not run on the development
> host (its sanitizer runtime is broken; see README) and is left to CI; the SPSC
> rings use standard acquire/release ordering.

## 1. Recommendation

Keep each `OrderBook` single-writer and give each of four books exactly one
long-lived worker thread. Add a thin, fixed-size routing facade above four existing
`OrderBookEngine` instances:

```text
producer / strategy threads
            |
            | RoutedOrderRequest { BookId, OrderRequest }
            v
FourBookEngine (routing and coordinated lifetime; no worker thread)
      |             |             |             |
      v             v             v             v
 book 0 queue    book 1 queue   book 2 queue   book 3 queue
      |             |             |             |
   worker 0       worker 1      worker 2      worker 3
      |             |             |             |
 OrderBook 0     OrderBook 1    OrderBook 2    OrderBook 3
```

The facade routes directly by `BookId`; it does not use a central dispatcher
thread. Requests for different books can execute concurrently, while requests for
one book remain serialized. The matching core gains no locks and requires no
matching-rule or data-structure change.

This is the lowest-risk first version because the existing `OrderBookEngine`
already provides the required one-queue, one-worker ownership model. A custom
bounded queue, CPU affinity, or busy-polling should be a later, measured
optimization rather than part of the first correctness change.

## 2. Goals and non-goals

### Goals

- Own exactly four independent order books and four long-lived worker threads.
- Preserve price-time priority and all existing matching semantics within each
  book.
- Allow any producer thread to submit to any book safely.
- Run work for different books concurrently without putting locks in
  `OrderBook`.
- Make routing, event identity, shutdown, and validation behavior explicit.
- Establish a multibook benchmark before changing the existing queue design.

### Non-goals for the first version

- Atomic operations spanning multiple books.
- A deterministic total order across books.
- Migrating a live book between workers.
- Dynamic book creation or a general-purpose thread pool.
- Lock-free queues, busy-spinning, CPU pinning, NUMA placement, or real-time
  scheduler policy.
- Concurrent direct reads of a live `OrderBook`.
- Persistence, recovery, networking, or cross-book risk checks.

These can be added around the four-book engine later. None is necessary to obtain
parallel matching across four independent instruments.

## 3. Public types and API boundary

### Book identity

Add a strong `BookId` type in the coordination layer. It has exactly four valid
values and converts to an array index only through a checked function. Raw symbol
strings and external numeric identifiers remain adapter concerns.

Use `BookId` on every operation, including cancel. An order ID identifies an order
only within one book, so the stable identity of an active order is:

```text
(BookId, OrderId)
```

The same `OrderId` may therefore be active in two different books. Duplicate
detection remains local to a book. An adapter that receives cancel-by-order-ID
without a book must maintain its own order-to-book routing index; the matching
engine should not add a shared global order index.

### Proposed facade

The precise names can change during review, but the intended shape is:

```cpp
inline constexpr std::size_t book_count{4};

struct FourBookConfig {
    std::array<BookConfig, book_count> books;
};

class FourBookEngine {
public:
    explicit FourBookEngine(FourBookConfig config);
    ~FourBookEngine();

    FourBookEngine(const FourBookEngine&) = delete;
    FourBookEngine& operator=(const FourBookEngine&) = delete;
    FourBookEngine(FourBookEngine&&) = delete;
    FourBookEngine& operator=(FourBookEngine&&) = delete;

    [[nodiscard]] std::future<SubmitResult>
    enqueue(BookId book, OrderRequest request);

    void shutdown();
    [[nodiscard]] bool accepting() const;

    // Available only after shutdown, matching the current engine contract.
    [[nodiscard]] const OrderBook&
    book_after_shutdown(BookId book) const noexcept;
};
```

Internally, `FourBookEngine` can own
`std::array<std::unique_ptr<OrderBookEngine>, book_count>`. The heap allocations
occur once during construction and avoid requiring the existing non-movable
`OrderBookEngine` to become movable. They do not affect matching-path allocation.

The first version should preserve the existing `std::future<SubmitResult>` API.
It is convenient for correctness, error propagation, and tests, although its
per-request promise/future cost is not suitable for an unqualified
low-latency-performance claim. The multibook benchmark will show whether it must
later be replaced by a bounded response channel or batched completion mechanism.

## 4. Ownership and concurrency contract

Each shard owns these resources exclusively:

- one command queue, mutex, and condition variable;
- one worker thread;
- one `OrderBook`;
- one order pool, order-ID index, and pair of price-level maps;
- one per-book execution sequence;
- one event-output path.

Only the shard's worker mutates its `OrderBook`. The existing
`std::pmr::unsynchronized_pool_resource` remains valid because it is not shared
between books and is touched by only that worker while live.

The routing facade performs only:

1. validate or resolve `BookId`;
2. select one of four shards;
3. call that shard's `enqueue`.

It does not hold a global lock during matching and does not move work between
shards. A busy book can accumulate queue depth without blocking another book's
worker, although producers can still contend on the busy book's queue mutex.

### Ordering guarantees

- Queue order and price-time priority are preserved independently per book.
- Requests routed to different books have no defined relative execution order.
- Two producer threads racing to enqueue to the same book are ordered by that
  book's queue admission; their wall-clock call order is not a sequencing
  guarantee.
- Cross-book results and events may arrive in any interleaving.
- A cross-book operation is never atomic in the first version.

These rules preserve deterministic replay when the replay log contains a
per-book accepted-command order. A single global replay sequence may be recorded
by an adapter if the competition protocol requires one, but it should not be used
to serialize the matching workers.

## 5. Events and execution identity

The current event types do not carry a book identifier, and the current
`RecordingEventSink` and `PrintingEventSink` are not safe to call concurrently
from four workers. Passing one existing sink instance to all four books would
therefore be a data race and is forbidden.

The correctness-first implementation should give every book a distinct sink.
Tests can use four separate `RecordingEventSink` instances. A live adapter can
associate each sink with its known `BookId`.

For a merged event feed, add a small per-book adapter that converts events to:

```text
RoutedExecutionEvent { BookId, ExecutionEvent }
RoutedRejectionEvent { BookId, RejectionEvent }
```

The preferred live path is one bounded single-producer/single-consumer output
queue per book. Each book worker is its queue's only producer, and an event
consumer polls the four queues. This prevents console I/O, allocation, or a shared
output mutex from entering matching callbacks. Queue-full behavior must be chosen
explicitly—rejecting input before mutation, dropping only nonessential telemetry,
or applying backpressure are materially different semantics.

`SequenceNumber` remains per book. The globally unique execution identity is
`(BookId, SequenceNumber)`. Creating one shared atomic execution counter would add
cross-core contention and would not make the cross-book state atomic.

The per-book output queues are not required for the first routing milestone if
the application can consume four separate sinks safely. They are required before
claiming that a merged live event stream is safe and nonblocking.

## 6. Construction, failure, and shutdown

### Construction

Construct all per-book sinks before their engines because each sink must outlive
the corresponding book. Start one `OrderBookEngine` per book. If construction of
a later shard or thread fails, RAII destruction of already-created shards must
stop and join their workers before the exception escapes.

Creating four workers does not guarantee that four physical cores run them
simultaneously; the operating-system scheduler decides that. Portable C++20 has no
standard CPU-affinity API. Affinity is an optional platform adapter to evaluate
only after benchmarks show scheduler migration is a problem.

### Shutdown

`FourBookEngine::shutdown()` must be idempotent and safe against concurrent
submission:

1. stop global admission once;
2. prevent any new request from being routed;
3. call `shutdown()` on all four shards;
4. let each shard drain every command it accepted;
5. join all four workers;
6. expose the four books for read-only validation and inspection.

Stopping shards sequentially is correct because all four workers continue running
until their own join. A future improvement may initiate stop on all shards before
joining any of them, but it is not required for correctness.

An enqueue racing with shutdown must have one of two outcomes: it is admitted by
exactly one shard and completed before that shard joins, or it receives
`Status::engine_stopped`. It must never be silently lost.

Live book queries are deliberately deferred. The current safe rule remains:
inspect a book only after its worker has stopped. If the competition needs live
top-of-book reads, implement them as commands executed by the owning worker or as
published immutable snapshots; do not read the mutable maps from another thread.

## 7. Expected implementation impact

### What should not change

- matching algorithms in `src/order_book.cpp`;
- order-pool, ID-index, or price-level data structures;
- price-time priority, time-in-force, cancellation, and rejection semantics;
- the rule that `OrderBook` contains no locks;
- allocation behavior inside one book.

### Expected additions or edits

| Area | Likely change |
|---|---|
| Domain/routing types | Add `BookId` and checked adapter conversion |
| Coordination API | Add `FourBookEngine` facade and four-shard configuration |
| Implementation | Construct, route to, stop, join, and inspect four engines |
| Events | Document/enforce distinct sinks; optionally add tagged sink adapters |
| Tests | Add deterministic routing, isolation, lifecycle, and concurrency tests |
| Benchmark | Add a separate four-book coordination benchmark |
| CMake | Register new source, tests, and benchmark targets |
| Documentation | Update architecture, benchmarking method, and README when implemented |

The minimal routing version is a **moderate coordination-layer change, not a major
matching-engine rewrite**. Most production code can reuse `OrderBookEngine`.
The difficult part is verifying concurrency boundaries rather than writing four
`std::thread` objects.

Relative effort:

| Scope | Difficulty | Reason |
|---|---|---|
| Four books + four workers by composition | Low to moderate | Existing one-worker engine is reusable |
| Correct routing, shutdown, and deterministic tests | Moderate | Races and lifetime rules need explicit coverage |
| Safe merged event stream | Moderate | Requires tagging and a concurrency/backpressure policy |
| Measured queue optimization and CPU affinity | Moderate to high | Workload- and platform-dependent |
| Cross-book atomicity or globally consistent live snapshots | High | Requires a separate coordination protocol |

For an experienced C++ developer, the correctness-first facade is plausibly a
small, focused implementation (roughly a few production files plus a larger test
change). Competition hardening—events, overload behavior, multithreaded benchmark,
profiling, and platform tuning—is at least as important as the facade and should
be treated as a separate phase. This assessment assumes the four books are
independent instruments; cross-book transactional behavior would make the change
substantially larger.

## 8. Test plan

All mutation-heavy tests must validate every book after shutdown.

### Routing and isolation

- Route a distinct resting order to each book and verify it appears only there.
- Accept the same `OrderId` simultaneously in all four books.
- Reject a duplicate `OrderId` within one book without affecting the other three.
- Cancel using `(BookId, OrderId)` and verify that only the selected book changes.
- Execute trades at different prices in all four books and verify independent
  sequence numbers and event attribution.

### Concurrency and progress

- Submit concurrently to all four books using a start barrier, without real-time
  sleeps.
- Use deterministic per-book request streams with precomputed expected states.
- Verify a heavily loaded book does not change or corrupt another book.
- Exercise multiple producers targeting one shard and producers distributed over
  all shards.
- Keep callbacks separate per shard so TSan can distinguish engine races from an
  intentionally unsafe test sink.

### Lifecycle

- Verify construction/destruction with no submitted work.
- Verify shutdown drains accepted work on all four books.
- Verify shutdown is idempotent.
- Race submissions with shutdown and account for every request as completed or
  `engine_stopped`.
- Verify all post-shutdown submissions return `engine_stopped`.
- Validate all four books after shutdown.

### Required verification

- Strict debug and release builds with warnings as errors.
- Full CTest suite.
- ASan and UBSan.
- TSan for the new four-worker tests on a supported runtime.
- Repeated lifecycle/concurrency tests to catch intermittent hangs or lost work.

## 9. Benchmark plan

Do not compare the four-worker facade directly with the existing direct-core
benchmark; they measure different layers. Add a separate release benchmark with:

- four pre-generated deterministic request streams;
- one `NullEventSink` per book;
- fixed warmup and measured phases;
- a start barrier for producer timing;
- final invariant validation and per-book checksums;
- aggregate operations/second and per-book operations/second;
- enqueue-to-completion p50/p95/p99 latency;
- queue-depth or overload observations;
- the existing mutex/condition-variable/future implementation identified in the
  output.

Measure at least these workload shapes:

1. balanced: 25% of requests to each book;
2. skewed: 70–90% to one hot book;
3. independent producers: one producer per book;
4. contended producers: multiple producers submitting to all books;
5. single-book control using the same coordination API.

Record CPU model, logical-core count, OS, compiler, flags, request count, and
whether affinity was used. Four workers can improve aggregate throughput only
when the machine has enough schedulable cores and the producer/completion path is
not the bottleneck. The benchmark, rather than thread count alone, determines
whether the architecture helps the competition workload.

## 10. Implementation phases

### Phase 1: correctness-first composition

1. Finalize `BookId`, order-ID scope, and per-book ordering semantics.
2. Add `FourBookEngine` by composing four `OrderBookEngine` instances.
3. Require distinct per-book event sinks.
4. Add routing, isolation, concurrency, and shutdown tests.
5. Run strict builds, CTest, sanitizers, and TSan where supported.
6. Synchronize `docs/architecture.md` and `README.md` with implemented behavior.

### Phase 2: representative multibook benchmark

1. Add the separate coordination benchmark described above.
2. Establish balanced and skewed baselines.
3. Measure aggregate scaling, completion latency, allocations, and contention.
4. Document results in `docs/benchmarking.md` without overwriting the
   direct-core baseline.

### Phase 3: competition adapter and measured optimization

1. Add book-tagged event output and decide bounded-queue overflow semantics.
2. Profile the actual competition request distribution.
3. If evidence supports it, compare futures against a bounded response channel or
   batched completions.
4. Evaluate busy-polling or platform-specific CPU affinity only as isolated
   benchmark variants.
5. Promote only variants that keep tests and sanitizers green and improve the
   representative workload.

## 11. Review decisions

The following choices should be confirmed before implementation:

1. Which four instruments map to the four stable `BookId` values?
2. Are order IDs unique per book, as recommended, or globally assigned by the
   competition protocol?
3. Does every cancel request include its book/instrument?
4. Are per-book execution sequences acceptable, or does an external adapter need
   to stamp a global observation sequence?
5. Can consumers handle four separate event streams, or is a merged stream
   required?
6. If an event/output queue fills, may telemetry be dropped, must producers block,
   or must new input be rejected before matching?
7. Is reading books only after shutdown sufficient for the first version, or are
   live top-of-book snapshots required?
8. What hardware and request distribution will be used in the competition?

The answers mostly affect the coordination and adapter layers. They should not
change the single-writer matching core.
