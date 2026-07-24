// Phase 3 demo: four books matched in parallel, with their executions merged into a
// single book-tagged feed by one consumer thread over bounded lock-free SPSC rings.
// Shows that a live consumer can observe all four books through one stream without
// sharing a sink across workers or locking the matching path.
#include "orderbook/event_stream.hpp"
#include "orderbook/four_book_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace orderbook;

constexpr std::array<BookId, book_count> books{BookId::book_0, BookId::book_1,
                                              BookId::book_2, BookId::book_3};
constexpr int trades_per_book{2'000};
constexpr Price trade_price{1'000'000};

}  // namespace

int main() {
    MergedEventStream stream{trades_per_book};  // ring per book large enough for no drops
    FourBookConfig config;
    for (std::size_t i{0}; i < book_count; ++i) {
        config.books[i] = BookConfig{2 * trades_per_book + 10, &stream.sink(static_cast<BookId>(i))};
    }
    FourBookEngine engine{config};

    std::cout << "FourBookEngine + merged event stream: 4 books, 4 workers, "
              << "one consumer merging all executions.\n\n";

    // One consumer thread drains the four rings into a single tagged feed.
    std::array<std::atomic<long>, book_count> merged_per_book{};
    std::atomic<long> total_merged{0};
    std::atomic<bool> done{false};
    const auto consume = [&](const RoutedEvent& event) {
        if (event.kind == RoutedEvent::Kind::execution) {
            merged_per_book[index_of(event.book)].fetch_add(1, std::memory_order_relaxed);
            total_merged.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread consumer{[&] {
        while (!done.load(std::memory_order_acquire)) {
            static_cast<void>(stream.drain(consume));
        }
    }};

    // One producer per book, each generating `trades_per_book` crossing trades.
    std::vector<std::thread> producers;
    producers.reserve(book_count);
    for (std::size_t book_index{0}; book_index < book_count; ++book_index) {
        producers.emplace_back([&, book_index] {
            const BookId book{books[book_index]};
            for (int k{0}; k < trades_per_book; ++k) {
                static_cast<void>(engine.enqueue(book, LimitOrder{
                    OrderId{static_cast<std::uint64_t>(2 * k + 1)}, Side::sell, trade_price, 1}));
                static_cast<void>(engine.enqueue(book, LimitOrder{
                    OrderId{static_cast<std::uint64_t>(2 * k + 2)}, Side::buy, trade_price, 1}));
            }
        });
    }
    for (auto& producer : producers) producer.join();

    engine.shutdown();  // drain and join the four workers
    done.store(true, std::memory_order_release);
    consumer.join();
    static_cast<void>(stream.drain(consume));  // final sweep, single consumer

    int exit_code{0};
    long expected_total{0};
    for (std::size_t i{0}; i < book_count; ++i) {
        std::cout << "book " << i << ": merged_executions=" << merged_per_book[i].load()
                  << " dropped=" << stream.dropped(books[i]) << '\n';
        expected_total += trades_per_book;
        if (merged_per_book[i].load() != trades_per_book || stream.dropped(books[i]) != 0) {
            exit_code = 1;
        }
    }
    std::cout << "\nmerged feed total executions=" << total_merged.load()
              << " (expected " << expected_total << ")\n";
    if (total_merged.load() != expected_total) exit_code = 1;
    return exit_code;
}
