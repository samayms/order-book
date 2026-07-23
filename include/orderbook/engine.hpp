#pragma once

#include "orderbook/order_book.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <variant>

namespace orderbook {

using OrderRequest = std::variant<LimitOrder, MarketOrder, CancelOrder>;

class OrderBookEngine {
public:
    explicit OrderBookEngine(BookConfig config = {});
    ~OrderBookEngine();

    OrderBookEngine(const OrderBookEngine&) = delete;
    OrderBookEngine& operator=(const OrderBookEngine&) = delete;
    OrderBookEngine(OrderBookEngine&&) = delete;
    OrderBookEngine& operator=(OrderBookEngine&&) = delete;

    [[nodiscard]] std::future<SubmitResult> enqueue(OrderRequest request);
    void shutdown();
    [[nodiscard]] bool accepting() const;

    // The caller must invoke shutdown() before reading the underlying book.
    [[nodiscard]] const OrderBook& book_after_shutdown() const noexcept { return book_; }

private:
    struct Command {
        OrderRequest request;
        std::promise<SubmitResult> completion;
    };

    void run();
    [[nodiscard]] SubmitResult dispatch(const OrderRequest& request);

    OrderBook book_;
    mutable std::mutex queue_mutex_;
    std::condition_variable command_ready_;
    std::queue<Command> commands_;
    bool accepting_{true};
    std::once_flag shutdown_once_;
    std::thread worker_;
};

}  // namespace orderbook
