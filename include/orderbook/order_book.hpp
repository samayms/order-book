#pragma once

#include "orderbook/events.hpp"
#include "orderbook/order_index.hpp"
#include "orderbook/order_pool.hpp"
#include "orderbook/types.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace orderbook {

struct BookConfig {
    std::size_t maximum_orders{100'000};
    EventSink* event_sink{nullptr};
};

struct OrderView {
    OrderId id;
    Side side{Side::buy};
    Price price;
    Quantity remaining_quantity{0};
};

struct LevelView {
    Price price;
    std::uint32_t order_count{0};
    AggregateQuantity total_quantity{0};
};

struct BookStats {
    std::size_t active_orders{0};
    std::size_t bid_levels{0};
    std::size_t ask_levels{0};
    SequenceNumber execution_count{0};
};

struct ValidationResult {
    bool valid{true};
    std::string message;
};

class OrderBook {
public:
    explicit OrderBook(BookConfig config = {});

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] SubmitResult submit(const LimitOrder& order);
    [[nodiscard]] SubmitResult submit(const MarketOrder& order) noexcept;
    [[nodiscard]] Status cancel(OrderId id) noexcept;

    [[nodiscard]] std::optional<OrderView> find(OrderId id) const noexcept;
    [[nodiscard]] std::optional<LevelView> best_bid() const noexcept;
    [[nodiscard]] std::optional<LevelView> best_ask() const noexcept;
    [[nodiscard]] std::optional<LevelView> level(Side side, Price price) const noexcept;
    [[nodiscard]] BookStats stats() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.capacity(); }
    [[nodiscard]] ValidationResult validate() const;

private:
    struct Level {
        OrderHandle head{invalid_handle};
        OrderHandle tail{invalid_handle};
        std::uint32_t order_count{0};
        AggregateQuantity total_quantity{0};
    };

    struct IncomingOrder {
        OrderId id;
        Side side{Side::buy};
        Quantity remaining_quantity{0};
    };

    using BidLevels = std::map<Price, Level, std::greater<Price>>;
    using AskLevels = std::map<Price, Level>;

    [[nodiscard]] SubmitResult reject(OrderId id, Status status,
                                      Quantity remaining) noexcept;
    [[nodiscard]] SubmitResult match(IncomingOrder& incoming,
                                     std::optional<Price> limit_price) noexcept;
    [[nodiscard]] AggregateQuantity available_quantity(
        Side taker_side, Price limit_price, AggregateQuantity stop_after) const noexcept;
    void append(OrderHandle handle, Level& level) noexcept;
    void unlink(OrderHandle handle, Level& level) noexcept;
    [[nodiscard]] static LevelView make_level_view(Price price,
                                                   const Level& level) noexcept;

    NullEventSink null_sink_;
    EventSink* event_sink_;
    OrderPool pool_;
    OrderIndex index_;
    BidLevels bids_;
    AskLevels asks_;
    SequenceNumber next_execution_sequence_{1};
};

}  // namespace orderbook
