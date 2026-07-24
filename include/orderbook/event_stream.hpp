#pragma once

#include "orderbook/book_id.hpp"
#include "orderbook/events.hpp"
#include "orderbook/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace orderbook {

// Book-tagged events for a single merged feed. The globally-unique execution
// identity is (book, execution.sequence); each book keeps its own SequenceNumber.
struct RoutedEvent {
    enum class Kind : std::uint8_t { execution, rejection };
    Kind kind{Kind::execution};
    BookId book{BookId::book_0};
    ExecutionEvent execution{};
    RejectionEvent rejection{};
};

// Bounded lock-free single-producer/single-consumer ring. Exactly one thread may
// push (a book's worker) and exactly one may pop (the merge consumer). Preallocated
// at construction; push never allocates or blocks, so it is safe inside a matching
// callback. Full push returns false — the caller decides the overflow policy.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : slots_(round_up_pow2(capacity)), mask_{slots_.size() - 1} {}

    [[nodiscard]] bool push(const T& value) noexcept {
        const std::size_t tail{tail_.load(std::memory_order_relaxed)};
        if (tail - head_.load(std::memory_order_acquire) >= slots_.size()) {
            return false;  // full
        }
        slots_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t head{head_.load(std::memory_order_relaxed)};
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    [[nodiscard]] static std::size_t round_up_pow2(std::size_t n) noexcept {
        std::size_t power{2};
        while (power < n) power <<= 1U;
        return power;
    }

    std::vector<T> slots_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};  // written only by the consumer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written only by the producer
};

// EventSink handed to one book: tags each event with that book's id and pushes it
// into that book's ring. On overflow it drops the event and counts it (blocking or
// allocating in a matching callback is forbidden), so a slow consumer costs
// telemetry, never matching progress or correctness.
class RoutingEventSink final : public EventSink {
public:
    RoutingEventSink() = default;

    void bind(BookId book, SpscRing<RoutedEvent>* ring) noexcept {
        book_ = book;
        ring_ = ring;
    }

    void on_execution(const ExecutionEvent& event) noexcept override {
        if (ring_ != nullptr &&
            !ring_->push(RoutedEvent{RoutedEvent::Kind::execution, book_, event, {}})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void on_rejection(const RejectionEvent& event) noexcept override {
        if (ring_ != nullptr &&
            !ring_->push(RoutedEvent{RoutedEvent::Kind::rejection, book_, {}, event})) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    BookId book_{BookId::book_0};
    SpscRing<RoutedEvent>* ring_{nullptr};
    std::atomic<std::size_t> dropped_{0};
};

// Owns the four per-book rings and their routing sinks. Wire one sink into each
// book's BookConfig, then have a single consumer thread call drain() to receive a
// merged, book-tagged event feed. drain() must be called from only one thread at a
// time (the single consumer); it is otherwise safe to run concurrently with the
// four book workers.
class MergedEventStream {
public:
    explicit MergedEventStream(std::size_t per_book_capacity) {
        for (std::size_t i{0}; i < book_count; ++i) {
            rings_[i] = std::make_unique<SpscRing<RoutedEvent>>(per_book_capacity);
            sinks_[i].bind(static_cast<BookId>(i), rings_[i].get());
        }
    }

    MergedEventStream(const MergedEventStream&) = delete;
    MergedEventStream& operator=(const MergedEventStream&) = delete;
    MergedEventStream(MergedEventStream&&) = delete;
    MergedEventStream& operator=(MergedEventStream&&) = delete;

    // The sink to place in this book's BookConfig::event_sink.
    [[nodiscard]] EventSink& sink(BookId book) noexcept { return sinks_[index_of(book)]; }

    // Events dropped for a book because its ring was full (read after shutdown for a
    // stable total, or during the run as an approximate gauge).
    [[nodiscard]] std::size_t dropped(BookId book) const noexcept {
        return sinks_[index_of(book)].dropped();
    }

    // Non-blocking: deliver every event currently available across the four books to
    // on_event, returning how many were delivered. Single-consumer only.
    template <typename OnEvent>
    std::size_t drain(OnEvent&& on_event) {
        std::size_t delivered{0};
        RoutedEvent event;
        for (auto& ring : rings_) {
            while (ring->pop(event)) {
                on_event(event);
                ++delivered;
            }
        }
        return delivered;
    }

private:
    std::array<std::unique_ptr<SpscRing<RoutedEvent>>, book_count> rings_;
    std::array<RoutingEventSink, book_count> sinks_;
};

}  // namespace orderbook
