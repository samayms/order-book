#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orderbook {

class OrderIndex {
public:
    explicit OrderIndex(std::size_t maximum_entries);

    [[nodiscard]] bool insert(OrderId id, OrderHandle handle) noexcept;
    [[nodiscard]] std::optional<OrderHandle> find(OrderId id) const noexcept;
    [[nodiscard]] bool erase(OrderId id) noexcept;

    [[nodiscard]] bool contains(OrderId id) const noexcept { return find(id).has_value(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return maximum_entries_; }
    [[nodiscard]] std::size_t table_capacity() const noexcept { return entries_.size(); }

private:
    enum class EntryState : std::uint8_t { empty, occupied, tombstone };

    struct Entry {
        OrderId id;
        OrderHandle handle{invalid_handle};
        EntryState state{EntryState::empty};
    };

    [[nodiscard]] static std::uint64_t hash(OrderId id) noexcept;
    [[nodiscard]] std::size_t bucket(OrderId id) const noexcept;
    void rebuild() noexcept;
    static void insert_unchecked(std::vector<Entry>& entries, OrderId id,
                                 OrderHandle handle) noexcept;

    std::vector<Entry> entries_;
    std::vector<Entry> scratch_;
    std::size_t maximum_entries_{0};
    std::size_t size_{0};
    std::size_t tombstone_count_{0};
};

}  // namespace orderbook
