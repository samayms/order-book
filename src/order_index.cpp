#include "orderbook/order_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

// Rebuild the table when tombstones exceed table_size / divisor. Overridable at
// compile time so benchmark experiments can sweep the threshold. The default of
// 8 was the fastest measured divisor on the deterministic benchmark workload
// (Apple M4 Pro, Apple Clang 17, 2026-07): sweep of {1,2,4,6,8,12,16,32} showed
// a peak at 8 (~+3-6% throughput vs 4); 1 (never rebuild) degrades throughput
// ~7x. Latency percentiles p50-p99.9 were indistinguishable between 4 and 8.
#ifndef ORDERBOOK_TOMBSTONE_REBUILD_DIVISOR
#define ORDERBOOK_TOMBSTONE_REBUILD_DIVISOR 8
#endif

namespace orderbook {

namespace {

inline constexpr std::size_t tombstone_rebuild_divisor{
    ORDERBOOK_TOMBSTONE_REBUILD_DIVISOR};
static_assert(tombstone_rebuild_divisor >= 1);

[[nodiscard]] std::size_t table_size_for(std::size_t maximum_entries) {
    if (maximum_entries == 0 ||
        maximum_entries > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::invalid_argument{"invalid order-index capacity"};
    }
    const std::size_t minimum{maximum_entries * 2};
    std::size_t table_size{8};
    while (table_size < minimum) {
        if (table_size > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::length_error{"order-index table size overflow"};
        }
        table_size *= 2;
    }
    return table_size;
}

}  // namespace

OrderIndex::OrderIndex(std::size_t maximum_entries)
    : entries_(table_size_for(maximum_entries)),
      scratch_(entries_.size()),
      maximum_entries_{maximum_entries} {}

std::uint64_t OrderIndex::hash(OrderId id) noexcept {
    std::uint64_t value{id.value()};
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::size_t OrderIndex::bucket(OrderId id) const noexcept {
    return static_cast<std::size_t>(hash(id)) & (entries_.size() - 1);
}

bool OrderIndex::insert(OrderId id, OrderHandle handle) noexcept {
    if (!id.valid() || size_ >= maximum_entries_) {
        return false;
    }
    if (tombstone_count_ > entries_.size() / tombstone_rebuild_divisor) {
        rebuild();
    }

    std::size_t first_tombstone{entries_.size()};
    std::size_t index{bucket(id)};
    for (std::size_t probes{0}; probes < entries_.size(); ++probes) {
        Entry& entry{entries_[index]};
        if (entry.state == EntryState::occupied && entry.id == id) {
            return false;
        }
        if (entry.state == EntryState::tombstone && first_tombstone == entries_.size()) {
            first_tombstone = index;
        }
        if (entry.state == EntryState::empty) {
            const std::size_t destination{
                first_tombstone == entries_.size() ? index : first_tombstone};
            entries_[destination] = Entry{id, handle, EntryState::occupied};
            if (first_tombstone != entries_.size()) {
                --tombstone_count_;
            }
            ++size_;
            return true;
        }
        index = (index + 1) & (entries_.size() - 1);
    }

    if (first_tombstone != entries_.size()) {
        entries_[first_tombstone] = Entry{id, handle, EntryState::occupied};
        --tombstone_count_;
        ++size_;
        return true;
    }
    return false;
}

OrderIndex::Reservation OrderIndex::reserve(OrderId id) noexcept {
    // The caller validates the id. The physical table has at least twice the
    // configured logical capacity, so an empty slot remains available even when
    // a later pool allocation rejects the request. Mirrors insert()'s probe but
    // writes nothing; commit() finalises the chosen slot only for a remainder.
    if (tombstone_count_ > entries_.size() / tombstone_rebuild_divisor) {
        rebuild();
    }
    std::size_t first_tombstone{entries_.size()};
    std::size_t index{bucket(id)};
    for (std::size_t probes{0}; probes < entries_.size(); ++probes) {
        const Entry& entry{entries_[index]};
        if (entry.state == EntryState::occupied && entry.id == id) {
            return Reservation{index, true};
        }
        if (entry.state == EntryState::tombstone && first_tombstone == entries_.size()) {
            first_tombstone = index;
        }
        if (entry.state == EntryState::empty) {
            return Reservation{
                first_tombstone == entries_.size() ? index : first_tombstone, false};
        }
        index = (index + 1) & (entries_.size() - 1);
    }
    return Reservation{first_tombstone, false};
}

void OrderIndex::commit(Reservation reservation, OrderId id, OrderHandle handle) noexcept {
    Entry& entry{entries_[reservation.slot]};
    if (entry.state == EntryState::tombstone) {
        --tombstone_count_;
    }
    entry = Entry{id, handle, EntryState::occupied};
    ++size_;
}

std::optional<OrderHandle> OrderIndex::find(OrderId id) const noexcept {
    if (!id.valid()) {
        return std::nullopt;
    }
    std::size_t index{bucket(id)};
    for (std::size_t probes{0}; probes < entries_.size(); ++probes) {
        const Entry& entry{entries_[index]};
        if (entry.state == EntryState::empty) {
            return std::nullopt;
        }
        if (entry.state == EntryState::occupied && entry.id == id) {
            return entry.handle;
        }
        index = (index + 1) & (entries_.size() - 1);
    }
    return std::nullopt;
}

bool OrderIndex::erase(OrderId id) noexcept {
    if (!id.valid()) {
        return false;
    }
    std::size_t index{bucket(id)};
    for (std::size_t probes{0}; probes < entries_.size(); ++probes) {
        Entry& entry{entries_[index]};
        if (entry.state == EntryState::empty) {
            return false;
        }
        if (entry.state == EntryState::occupied && entry.id == id) {
            entry = Entry{.state = EntryState::tombstone};
            --size_;
            ++tombstone_count_;
            return true;
        }
        index = (index + 1) & (entries_.size() - 1);
    }
    return false;
}

void OrderIndex::insert_unchecked(
    std::vector<Entry>& entries, OrderId id, OrderHandle handle) noexcept {
    std::size_t index{static_cast<std::size_t>(hash(id)) & (entries.size() - 1)};
    while (entries[index].state == EntryState::occupied) {
        index = (index + 1) & (entries.size() - 1);
    }
    entries[index] = Entry{id, handle, EntryState::occupied};
}

void OrderIndex::rebuild() noexcept {
    std::fill(scratch_.begin(), scratch_.end(), Entry{});
    for (const Entry& entry : entries_) {
        if (entry.state == EntryState::occupied) {
            insert_unchecked(scratch_, entry.id, entry.handle);
        }
    }
    entries_.swap(scratch_);
    tombstone_count_ = 0;
}

}  // namespace orderbook
