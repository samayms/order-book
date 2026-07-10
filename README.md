# order-book

An in-memory limit order book matching engine written in C++. Orders are
submitted onto a thread-safe queue and applied to the book by a dedicated
worker thread, keeping all book mutation single-threaded while submission can
happen concurrently. It supports limit and market orders, order cancellation,
and price-time (FIFO) priority matching across multiple price levels.

## Features

- **Limit orders** — rest on the book if they don't fully match; fill against
  the best available opposing price otherwise.
- **Market orders** — fill against the book at any price; report unfilled
  quantity when liquidity runs out.
- **Cancellation** — lazy cancel via an `is_active` flag; cancelled orders are
  skipped and pruned during matching.
- **Price-time priority** — best price first, FIFO within a price level.
- **Price improvement** — a crossing order fills at the resting (maker's)
  price, not its own limit.
- **Concurrent submission** — orders are enqueued from any thread and applied
  serially by a single worker, so the book itself needs no internal locking.

## Architecture

Order submission and order matching are decoupled through a queue:

```
producers ──enqueueOrder()──▶ order_queue ──processOrders()──▶ OrderBook
   (any thread)              (mutex + condvar)   (single worker thread)
```

- **`OrderRequest`** — a plain value describing one action (`LIMIT`, `MARKET`,
  or `CANCEL`) plus its fields.
- **`enqueueOrder`** — pushes a request under `queueMutex` and notifies the
  worker via `queueCV`.
- **`processOrders`** — the worker loop. It blocks on the condition variable
  until a request is available or `done` is set, pops one request, and
  dispatches it to the matching `OrderBook` method. Because this is the only
  code that touches the book, matching stays lock-free internally.
- **`done`** — an `atomic<bool>` shutdown flag; once set and the queue drains,
  the worker exits and can be `join`ed.

### OrderBook internals

The book itself is built around a few coordinated data structures:

| Structure | Type | Purpose |
|-----------|------|---------|
| `order_map` | `unordered_map<string, shared_ptr<Order>>` | Look up active orders by ID (for cancel / query). |
| `bid_levels` / `ask_levels` | `unordered_map<double, deque<shared_ptr<Order>>>` | FIFO queue of resting orders at each price level. |
| `bids` | `priority_queue<double>` (max-heap) | Track the best (highest) bid price. |
| `asks` | `priority_queue<double, …, greater<double>>` (min-heap) | Track the best (lowest) ask price. |

Orders are held by `shared_ptr`, so an order stays alive in a price level's
`deque` even after it's erased from `order_map`. Cancellation flips
`is_active` to `false`; the matching loop lazily pops inactive orders off the
front of each level (`clean_front`), and empty levels are removed from the
heap on the fly.

Matching (`match_against`) is templated over the heap comparator so the same
logic drives both buy-side and sell-side matching. Fills are printed to
stdout as `FILL:` lines; market orders that can't be fully filled print a
`PARTIAL:` line.

## API

```cpp
OrderBook book;

// Direct calls
book.add_limit_order(id, is_buy, price, quantity);  // rest or match a limit order
book.add_market_order(id, is_buy, quantity);        // match at any price
book.cancel_order(id);                              // cancel a resting order
book.get_order_quantity(id);                        // remaining qty, or -1 if not active
book.active_order_count();                          // number of active orders

// Or via the queue (applied by the worker thread)
enqueueOrder({ OrderType::LIMIT,  "L1", /*is_buy=*/false, /*price=*/100.0, /*qty=*/10 });
enqueueOrder({ OrderType::MARKET, "M1", /*is_buy=*/true,  /*price=*/0.0,   /*qty=*/12 });
enqueueOrder({ OrderType::CANCEL, "L1" });
```

## Build & Run

The threaded version requires linking pthreads:

```sh
# Build and run the demo in main.cpp
g++ -std=c++17 -O2 -pthread main.cpp -o order_book
./order_book

# Build and run the test suite
g++ -std=c++17 -O2 -pthread test.cpp -o test
./test
```

The test harness (`test.cpp`) includes `main.cpp` directly (renaming its
`main` out of the way) and captures stdout to assert on `FILL`/`PARTIAL`
output. It covers no-match resting, exact/partial fills, price improvement,
price-time priority, multi-level fills, cancellation, and market orders.
