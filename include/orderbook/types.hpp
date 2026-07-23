#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace orderbook {

class OrderId {
public:
    constexpr OrderId() noexcept = default;
    explicit constexpr OrderId(std::uint64_t value) noexcept : value_{value} {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    auto operator<=>(const OrderId&) const = default;

private:
    std::uint64_t value_{0};
};

class Price {
public:
    constexpr Price() noexcept = default;
    explicit constexpr Price(std::int64_t ticks) noexcept : ticks_{ticks} {}

    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return ticks_ > 0; }

    auto operator<=>(const Price&) const = default;

private:
    std::int64_t ticks_{0};
};

using Quantity = std::uint32_t;
using AggregateQuantity = std::uint64_t;
using SequenceNumber = std::uint64_t;
using OrderHandle = std::uint32_t;

inline constexpr std::int64_t tick_scale{10'000};
inline constexpr OrderHandle invalid_handle{std::numeric_limits<OrderHandle>::max()};

enum class Side : std::uint8_t { buy, sell };
enum class TimeInForce : std::uint8_t { gtc, ioc, fok };

enum class Status : std::uint8_t {
    accepted,
    not_found,
    duplicate_order_id,
    invalid_order_id,
    invalid_price,
    invalid_quantity,
    capacity_exceeded,
    would_not_fully_fill,
    engine_stopped
};

struct LimitOrder {
    OrderId id;
    Side side{Side::buy};
    Price price;
    Quantity quantity{0};
    TimeInForce time_in_force{TimeInForce::gtc};
};

struct MarketOrder {
    OrderId id;
    Side side{Side::buy};
    Quantity quantity{0};
};

struct CancelOrder {
    OrderId id;
};

struct SubmitResult {
    Status status{Status::accepted};
    Quantity executed_quantity{0};
    Quantity remaining_quantity{0};
    std::uint32_t execution_count{0};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return status == Status::accepted;
    }
};

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::buy ? Side::sell : Side::buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    return side == Side::buy ? "buy" : "sell";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce value) noexcept {
    switch (value) {
        case TimeInForce::gtc: return "gtc";
        case TimeInForce::ioc: return "ioc";
        case TimeInForce::fok: return "fok";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(Status status) noexcept {
    switch (status) {
        case Status::accepted: return "accepted";
        case Status::not_found: return "not_found";
        case Status::duplicate_order_id: return "duplicate_order_id";
        case Status::invalid_order_id: return "invalid_order_id";
        case Status::invalid_price: return "invalid_price";
        case Status::invalid_quantity: return "invalid_quantity";
        case Status::capacity_exceeded: return "capacity_exceeded";
        case Status::would_not_fully_fill: return "would_not_fully_fill";
        case Status::engine_stopped: return "engine_stopped";
    }
    return "unknown";
}

[[nodiscard]] inline std::optional<Price> price_from_double(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    const long double scaled{static_cast<long double>(value) * tick_scale};
    const long double maximum{
        static_cast<long double>(std::numeric_limits<std::int64_t>::max())};
    if (scaled > maximum) {
        return std::nullopt;
    }
    const auto ticks{static_cast<std::int64_t>(std::round(scaled))};
    return ticks > 0 ? std::optional<Price>{Price{ticks}} : std::nullopt;
}

[[nodiscard]] constexpr double price_to_double(Price price) noexcept {
    return static_cast<double>(price.ticks()) / static_cast<double>(tick_scale);
}

}  // namespace orderbook
