#include "orderbook/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orderbook {

namespace {

[[nodiscard]] std::size_t checked_capacity(std::size_t capacity) {
    if (capacity == 0 || capacity >= static_cast<std::size_t>(invalid_handle)) {
        throw std::invalid_argument{"maximum_orders must fit a nonzero OrderHandle range"};
    }
    return capacity;
}

}  // namespace

OrderBook::OrderBook(BookConfig config)
    : event_sink_{config.event_sink == nullptr
          ? static_cast<EventSink*>(&null_sink_)
          : config.event_sink},
      pool_{checked_capacity(config.maximum_orders)},
      index_{checked_capacity(config.maximum_orders)} {}

SubmitResult OrderBook::reject(
    OrderId id, Status status, Quantity remaining) noexcept {
    event_sink_->on_rejection(RejectionEvent{id, status});
    return SubmitResult{status, 0, remaining, 0};
}

SubmitResult OrderBook::submit(const LimitOrder& order) {
    if (!order.id.valid()) {
        return reject(order.id, Status::invalid_order_id, order.quantity);
    }
    if (!order.price.valid()) {
        return reject(order.id, Status::invalid_price, order.quantity);
    }
    if (order.quantity == 0) {
        return reject(order.id, Status::invalid_quantity, 0);
    }

    if (order.time_in_force == TimeInForce::fok) {
        if (index_.contains(order.id)) {
            return reject(order.id, Status::duplicate_order_id, order.quantity);
        }
        const AggregateQuantity available{
            available_quantity(order.side, order.price, order.quantity)};
        if (available < order.quantity) {
            return reject(order.id, Status::would_not_fully_fill, order.quantity);
        }
        IncomingOrder incoming{order.id, order.side, order.quantity};
        return match(incoming, order.price);
    }

    if (order.time_in_force == TimeInForce::ioc) {
        if (index_.contains(order.id)) {
            return reject(order.id, Status::duplicate_order_id, order.quantity);
        }
        IncomingOrder incoming{order.id, order.side, order.quantity};
        return match(incoming, order.price);
    }

    // GTC: one index probe (reserve) both rejects a duplicate and locates the
    // slot to write if the order rests. Duplicate is detected before the pool
    // allocation so it keeps precedence over capacity_exceeded; the reserved
    // slot is committed in O(1) only if a remainder rests.
    const auto reservation{index_.reserve(order.id)};
    if (reservation.duplicate) {
        return reject(order.id, Status::duplicate_order_id, order.quantity);
    }

    const auto handle{pool_.allocate(order.id, order.side, order.price, order.quantity)};
    if (!handle.has_value()) {
        return reject(order.id, Status::capacity_exceeded, order.quantity);
    }

    // A GTC that cannot cross the current best opposite price rests in full
    // without executing; skip the matching machinery entirely for it.
    const bool crosses{order.side == Side::buy
        ? (!asks_.empty() && asks_.begin()->first <= order.price)
        : (!bids_.empty() && bids_.begin()->first >= order.price)};

    auto process_gtc = [&](auto& own_levels) {
        const auto insertion_result{[&] {
            try {
                return own_levels.try_emplace(order.price);
            } catch (...) {
                pool_.release(*handle);
                throw;
            }
        }()};
        auto level_iterator{insertion_result.first};
        const bool inserted_level{insertion_result.second};

        if (!crosses) {
            index_.commit(reservation, order.id, *handle);
            append(*handle, level_iterator->second);
            return SubmitResult{Status::accepted, 0, order.quantity, 0};
        }

        IncomingOrder incoming{order.id, order.side, order.quantity};
        SubmitResult result{match(incoming, order.price)};
        if (incoming.remaining_quantity == 0) {
            pool_.release(*handle);
            if (inserted_level) {
                own_levels.erase(level_iterator);
            }
        } else {
            index_.commit(reservation, order.id, *handle);
            pool_.get(*handle).remaining_quantity = incoming.remaining_quantity;
            append(*handle, level_iterator->second);
        }
        return result;
    };

    return order.side == Side::buy ? process_gtc(bids_) : process_gtc(asks_);
}

SubmitResult OrderBook::submit(const MarketOrder& order) noexcept {
    if (!order.id.valid()) {
        return reject(order.id, Status::invalid_order_id, order.quantity);
    }
    if (order.quantity == 0) {
        return reject(order.id, Status::invalid_quantity, 0);
    }
    if (index_.contains(order.id)) {
        return reject(order.id, Status::duplicate_order_id, order.quantity);
    }
    IncomingOrder incoming{order.id, order.side, order.quantity};
    return match(incoming, std::nullopt);
}

SubmitResult OrderBook::match(
    IncomingOrder& incoming, std::optional<Price> limit_price) noexcept {
    const Quantity initial_quantity{incoming.remaining_quantity};
    std::uint32_t execution_count{0};

    auto execute = [&](auto& opposite_levels) {
        while (incoming.remaining_quantity > 0 && !opposite_levels.empty()) {
            auto level_iterator{opposite_levels.begin()};
            const Price maker_price{level_iterator->first};
            if (limit_price.has_value()) {
                const bool crosses{incoming.side == Side::buy
                    ? maker_price <= *limit_price
                    : maker_price >= *limit_price};
                if (!crosses) {
                    break;
                }
            }

            Level& price_level{level_iterator->second};
            while (incoming.remaining_quantity > 0 && price_level.head != invalid_handle) {
                const OrderHandle maker_handle{price_level.head};
                OrderNode& maker{pool_.get(maker_handle)};
                const Quantity executed{
                    std::min(incoming.remaining_quantity, maker.remaining_quantity)};

                incoming.remaining_quantity -= executed;
                maker.remaining_quantity -= executed;
                price_level.total_quantity -= executed;
                const ExecutionEvent event{
                    next_execution_sequence_, maker.id, incoming.id, incoming.side,
                    maker_price, executed};
                ++next_execution_sequence_;
                ++execution_count;

                if (maker.remaining_quantity == 0) {
                    unlink(maker_handle, price_level);
                    static_cast<void>(index_.erase(maker.id));
                    pool_.release(maker_handle);
                }
                event_sink_->on_execution(event);
            }

            if (price_level.order_count == 0) {
                opposite_levels.erase(level_iterator);
            }
        }
    };

    if (incoming.side == Side::buy) {
        execute(asks_);
    } else {
        execute(bids_);
    }

    return SubmitResult{
        Status::accepted,
        static_cast<Quantity>(initial_quantity - incoming.remaining_quantity),
        incoming.remaining_quantity,
        execution_count};
}

AggregateQuantity OrderBook::available_quantity(
    Side taker_side, Price limit_price, AggregateQuantity stop_after) const noexcept {
    AggregateQuantity available{0};
    auto accumulate = [&](const auto& opposite_levels) {
        for (const auto& [price, level] : opposite_levels) {
            const bool crosses{taker_side == Side::buy
                ? price <= limit_price
                : price >= limit_price};
            if (!crosses) {
                break;
            }
            available += level.total_quantity;
            if (available >= stop_after) {
                break;
            }
        }
    };
    if (taker_side == Side::buy) {
        accumulate(asks_);
    } else {
        accumulate(bids_);
    }
    return available;
}

void OrderBook::append(OrderHandle handle, Level& level) noexcept {
    OrderNode& order{pool_.get(handle)};
    order.previous = level.tail;
    order.next = invalid_handle;
    if (level.tail == invalid_handle) {
        level.head = handle;
    } else {
        pool_.get(level.tail).next = handle;
    }
    level.tail = handle;
    ++level.order_count;
    level.total_quantity += order.remaining_quantity;
}

void OrderBook::unlink(OrderHandle handle, Level& level) noexcept {
    OrderNode& order{pool_.get(handle)};
    if (order.previous == invalid_handle) {
        level.head = order.next;
    } else {
        pool_.get(order.previous).next = order.next;
    }
    if (order.next == invalid_handle) {
        level.tail = order.previous;
    } else {
        pool_.get(order.next).previous = order.previous;
    }
    level.total_quantity -= order.remaining_quantity;
    --level.order_count;
    order.previous = invalid_handle;
    order.next = invalid_handle;
}

Status OrderBook::cancel(OrderId id) noexcept {
    if (!id.valid()) {
        static_cast<void>(reject(id, Status::invalid_order_id, 0));
        return Status::invalid_order_id;
    }
    const auto handle{index_.find(id)};
    if (!handle.has_value()) {
        static_cast<void>(reject(id, Status::not_found, 0));
        return Status::not_found;
    }

    OrderNode& order{pool_.get(*handle)};
    // Cancelling a top-of-book order is common; when the containing price is
    // already begin() we take its iterator in O(1) instead of an O(log P) find.
    // A cancelled order's price is always present, so the map is non-empty.
    auto cancel_from = [&](auto& levels) {
        auto level_iterator{levels.begin()};
        if (level_iterator->first != order.price) {
            level_iterator = levels.find(order.price);
        }
        unlink(*handle, level_iterator->second);
        if (level_iterator->second.order_count == 0) {
            levels.erase(level_iterator);
        }
    };
    if (order.side == Side::buy) {
        cancel_from(bids_);
    } else {
        cancel_from(asks_);
    }
    static_cast<void>(index_.erase(id));
    pool_.release(*handle);
    return Status::accepted;
}

std::optional<OrderView> OrderBook::find(OrderId id) const noexcept {
    const auto handle{index_.find(id)};
    if (!handle.has_value()) {
        return std::nullopt;
    }
    const OrderNode& order{pool_.get(*handle)};
    return OrderView{order.id, order.side, order.price, order.remaining_quantity};
}

LevelView OrderBook::make_level_view(Price price, const Level& level) noexcept {
    return LevelView{price, level.order_count, level.total_quantity};
}

std::optional<LevelView> OrderBook::best_bid() const noexcept {
    return bids_.empty()
        ? std::nullopt
        : std::optional<LevelView>{make_level_view(bids_.begin()->first, bids_.begin()->second)};
}

std::optional<LevelView> OrderBook::best_ask() const noexcept {
    return asks_.empty()
        ? std::nullopt
        : std::optional<LevelView>{make_level_view(asks_.begin()->first, asks_.begin()->second)};
}

std::optional<LevelView> OrderBook::level(Side side, Price price) const noexcept {
    if (side == Side::buy) {
        const auto iterator{bids_.find(price)};
        return iterator == bids_.end()
            ? std::nullopt
            : std::optional<LevelView>{make_level_view(price, iterator->second)};
    }
    const auto iterator{asks_.find(price)};
    return iterator == asks_.end()
        ? std::nullopt
        : std::optional<LevelView>{make_level_view(price, iterator->second)};
}

BookStats OrderBook::stats() const noexcept {
    return BookStats{
        index_.size(), bids_.size(), asks_.size(), next_execution_sequence_ - 1};
}

ValidationResult OrderBook::validate() const {
    auto failure = [](std::string message) {
        return ValidationResult{false, std::move(message)};
    };
    std::vector<bool> seen(pool_.capacity(), false);
    std::size_t traversed{0};

    auto validate_levels = [&](const auto& levels, Side expected_side) -> ValidationResult {
        for (const auto& [price, level] : levels) {
            if (level.order_count == 0 || level.head == invalid_handle ||
                level.tail == invalid_handle) {
                return failure("empty or malformed price level");
            }
            OrderHandle current{level.head};
            OrderHandle previous{invalid_handle};
            std::uint32_t count{0};
            AggregateQuantity total{0};
            while (current != invalid_handle) {
                if (current >= seen.size() || !pool_.occupied(current) || seen[current]) {
                    return failure("invalid, free, or repeated order handle");
                }
                seen[current] = true;
                const OrderNode& order{pool_.get(current)};
                if (order.side != expected_side || order.price != price ||
                    order.remaining_quantity == 0 || order.previous != previous) {
                    return failure("order and containing level disagree");
                }
                const auto indexed_handle{index_.find(order.id)};
                if (!indexed_handle.has_value() || *indexed_handle != current) {
                    return failure("order index points to the wrong node");
                }
                total += order.remaining_quantity;
                previous = current;
                current = order.next;
                ++count;
                ++traversed;
                if (traversed > pool_.capacity()) {
                    return failure("cycle detected in intrusive order lists");
                }
            }
            if (previous != level.tail || count != level.order_count ||
                total != level.total_quantity) {
                return failure("price-level tail, count, or aggregate is incorrect");
            }
        }
        return ValidationResult{};
    };

    if (const ValidationResult bids_valid{validate_levels(bids_, Side::buy)};
        !bids_valid.valid) {
        return bids_valid;
    }
    if (const ValidationResult asks_valid{validate_levels(asks_, Side::sell)};
        !asks_valid.valid) {
        return asks_valid;
    }
    if (traversed != pool_.size() || traversed != index_.size()) {
        return failure("pool, index, and level order counts differ");
    }
    if (!bids_.empty() && !asks_.empty() &&
        bids_.begin()->first >= asks_.begin()->first) {
        return failure("book remains crossed after a public operation");
    }
    return ValidationResult{};
}

}  // namespace orderbook
