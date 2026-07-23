#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace orderbook {

struct OrderNode {
    OrderId id;
    Price price;
    Quantity remaining_quantity{0};
    Side side{Side::buy};
    OrderHandle previous{invalid_handle};
    OrderHandle next{invalid_handle};
    OrderHandle next_free{invalid_handle};
    bool occupied{false};
};

class OrderPool {
public:
    explicit OrderPool(std::size_t capacity);

    [[nodiscard]] std::optional<OrderHandle> allocate(
        OrderId id, Side side, Price price, Quantity quantity) noexcept;
    void release(OrderHandle handle) noexcept;

    [[nodiscard]] OrderNode& get(OrderHandle handle) noexcept { return nodes_[handle]; }
    [[nodiscard]] const OrderNode& get(OrderHandle handle) const noexcept {
        return nodes_[handle];
    }
    [[nodiscard]] bool occupied(OrderHandle handle) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool full() const noexcept { return size_ == nodes_.size(); }

private:
    std::vector<OrderNode> nodes_;
    OrderHandle free_head_{invalid_handle};
    std::size_t size_{0};
};

}  // namespace orderbook
