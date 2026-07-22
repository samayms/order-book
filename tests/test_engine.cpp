#include "orderbook/engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

using orderbook::BookConfig;
using orderbook::LimitOrder;
using orderbook::OrderBookEngine;
using orderbook::OrderId;
using orderbook::Price;
using orderbook::Side;
using orderbook::Status;

constexpr Price bid_price{990'000};

[[nodiscard]] constexpr OrderId id(std::uint64_t value) noexcept {
    return OrderId{value};
}

void require_valid(const OrderBookEngine& engine) {
    const auto validation{engine.book_after_shutdown().validate()};
    if (!validation.valid) {
        throw test::Failure{"book invariant failure: " + validation.message};
    }
}

}  // namespace

int main() {
    test::Suite suite;

    suite.run("shutdown drains accepted commands and is idempotent", [&] {
        OrderBookEngine engine{BookConfig{200, nullptr}};
        std::vector<std::future<orderbook::SubmitResult>> futures;
        for (std::uint64_t value{1}; value <= 100; ++value) {
            futures.push_back(engine.enqueue(
                LimitOrder{id(value), Side::buy, bid_price, 1}));
        }
        engine.shutdown();
        engine.shutdown();
        for (auto& future : futures) {
            suite.expect(future.get().accepted(), "accepted command did not finish during drain");
        }
        suite.equal(engine.book_after_shutdown().stats().active_orders, std::size_t{100},
                    "shutdown lost queued commands");
        suite.equal(engine.enqueue(LimitOrder{id(500), Side::buy, bid_price, 1}).get().status,
                    Status::engine_stopped, "post-shutdown request was admitted");
        require_valid(engine);
    });

    suite.run("multiple producers are serialized by one worker", [&] {
        constexpr int producer_count{4};
        constexpr int orders_per_producer{100};
        OrderBookEngine engine{BookConfig{
            static_cast<std::size_t>(producer_count * orders_per_producer + 10), nullptr}};
        std::atomic<int> accepted{0};
        std::vector<std::thread> producers;
        producers.reserve(producer_count);
        for (int producer{0}; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                std::vector<std::future<orderbook::SubmitResult>> futures;
                futures.reserve(orders_per_producer);
                for (int offset{0}; offset < orders_per_producer; ++offset) {
                    const auto numeric_id{static_cast<std::uint64_t>(
                        producer * orders_per_producer + offset + 1)};
                    futures.push_back(engine.enqueue(
                        LimitOrder{id(numeric_id), Side::buy, bid_price, 1}));
                }
                for (auto& future : futures) {
                    if (future.get().accepted()) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
        engine.shutdown();
        suite.equal(accepted.load(std::memory_order_relaxed),
                    producer_count * orders_per_producer,
                    "a concurrent producer command was rejected or lost");
        suite.equal(engine.book_after_shutdown().stats().active_orders,
                    static_cast<std::size_t>(producer_count * orders_per_producer),
                    "worker did not apply every producer command");
        require_valid(engine);
    });

    return suite.finish();
}
