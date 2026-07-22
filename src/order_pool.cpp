#include "orderbook/order_pool.hpp"

#include <stdexcept>

namespace orderbook {

OrderPool::OrderPool(std::size_t capacity) : nodes_(capacity) {
    if (capacity == 0 || capacity >= static_cast<std::size_t>(invalid_handle)) {
        throw std::invalid_argument{"order pool capacity is outside the handle range"};
    }
    for (std::size_t index{0}; index < capacity; ++index) {
        nodes_[index].next_free = index + 1 < capacity
            ? static_cast<OrderHandle>(index + 1)
            : invalid_handle;
    }
    free_head_ = 0;
}

std::optional<OrderHandle> OrderPool::allocate(
    OrderId id, Side side, Price price, Quantity quantity) noexcept {
    if (free_head_ == invalid_handle) {
        return std::nullopt;
    }

    const OrderHandle handle{free_head_};
    OrderNode& node{nodes_[handle]};
    free_head_ = node.next_free;
    node = OrderNode{
        .id = id,
        .price = price,
        .remaining_quantity = quantity,
        .side = side,
        .previous = invalid_handle,
        .next = invalid_handle,
        .next_free = invalid_handle,
        .occupied = true};
    ++size_;
    return handle;
}

void OrderPool::release(OrderHandle handle) noexcept {
    OrderNode& node{nodes_[handle]};
    node = OrderNode{.next_free = free_head_};
    free_head_ = handle;
    --size_;
}

bool OrderPool::occupied(OrderHandle handle) const noexcept {
    return handle < nodes_.size() && nodes_[handle].occupied;
}

}  // namespace orderbook
