#pragma once

#include "orderbook/book_id.hpp"
#include "orderbook/engine.hpp"
#include "orderbook/order_book.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>

namespace orderbook {

struct FourBookConfig {
    // One BookConfig per book. Each may carry a distinct event sink; sharing one
    // sink across books would be a data race across four workers and is a caller
    // error, not something the engine can make safe.
    std::array<BookConfig, book_count> books{};
};

// Routing facade over four independent single-writer OrderBookEngine shards. Each
// shard keeps its own queue, worker thread, book, pool, index, PMR resource, and
// event path; requests for different books run concurrently while requests for one
// book stay serialized in queue order. The facade adds no lock to the matching
// path and no dispatcher thread — it only validates a BookId and routes.
class FourBookEngine {
public:
    explicit FourBookEngine(const FourBookConfig& config);
    ~FourBookEngine();

    FourBookEngine(const FourBookEngine&) = delete;
    FourBookEngine& operator=(const FourBookEngine&) = delete;
    FourBookEngine(FourBookEngine&&) = delete;
    FourBookEngine& operator=(FourBookEngine&&) = delete;

    // Routes the request to the given book's shard. Once shutdown has begun the
    // request is not routed and completes with Status::engine_stopped; it is never
    // silently lost. Throws std::out_of_range for an invalid BookId (a programming
    // error, not an expected request failure).
    [[nodiscard]] std::future<SubmitResult> enqueue(BookId book, OrderRequest request);

    void shutdown();
    [[nodiscard]] bool accepting() const noexcept;

    // Valid only after shutdown(), matching the single-book engine contract: the
    // owning worker has stopped, so the book can be read without synchronization.
    [[nodiscard]] const OrderBook& book_after_shutdown(BookId book) const;

private:
    std::array<std::unique_ptr<OrderBookEngine>, book_count> shards_;
    std::atomic<bool> accepting_{true};
};

}  // namespace orderbook
