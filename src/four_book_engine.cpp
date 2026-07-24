#include "orderbook/four_book_engine.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace orderbook {

namespace {

[[nodiscard]] std::size_t checked_index(BookId book) {
    if (!valid(book)) {
        throw std::out_of_range{"FourBookEngine: invalid BookId"};
    }
    return static_cast<std::size_t>(book);
}

[[nodiscard]] Quantity remaining_of(const OrderRequest& request) noexcept {
    return std::visit(
        [](const auto& value) -> Quantity {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, CancelOrder>) {
                return 0;
            } else {
                return value.quantity;
            }
        },
        request);
}

[[nodiscard]] std::future<SubmitResult> stopped_future(const OrderRequest& request) {
    std::promise<SubmitResult> completion;
    std::future<SubmitResult> result{completion.get_future()};
    completion.set_value(SubmitResult{Status::engine_stopped, 0, remaining_of(request), 0});
    return result;
}

}  // namespace

FourBookEngine::FourBookEngine(const FourBookConfig& config) {
    // Construct shards in order. If a later shard throws (e.g. book construction or
    // thread creation fails), the already-created shards are destroyed by member
    // cleanup as the exception unwinds, each stopping and joining its worker.
    for (std::size_t index{0}; index < book_count; ++index) {
        shards_[index] = std::make_unique<OrderBookEngine>(config.books[index]);
    }
}

FourBookEngine::~FourBookEngine() {
    shutdown();
}

std::future<SubmitResult> FourBookEngine::enqueue(BookId book, OrderRequest request) {
    const std::size_t index{checked_index(book)};
    // Facade gate: once shutdown has begun, do not route — return engine_stopped so
    // the request is accounted for. If a request slips past this check concurrently
    // with shutdown, the shard's own gate still returns engine_stopped or drains it.
    if (!accepting_.load(std::memory_order_acquire)) {
        return stopped_future(request);
    }
    return shards_[index]->enqueue(std::move(request));
}

void FourBookEngine::shutdown() {
    // Idempotent and safe under concurrent calls: repeating the flag store is
    // harmless, and each shard's shutdown() is itself once-only and thread-safe.
    accepting_.store(false, std::memory_order_release);
    for (auto& shard : shards_) {
        if (shard) {
            shard->shutdown();
        }
    }
}

bool FourBookEngine::accepting() const noexcept {
    return accepting_.load(std::memory_order_acquire);
}

const OrderBook& FourBookEngine::book_after_shutdown(BookId book) const {
    return shards_[checked_index(book)]->book_after_shutdown();
}

}  // namespace orderbook
