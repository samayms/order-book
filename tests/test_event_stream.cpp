#include "orderbook/event_stream.hpp"
#include "orderbook/four_book_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"
#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace orderbook;

constexpr Price trade_price{1'000'000};
constexpr std::array<BookId, book_count> all_books{BookId::book_0, BookId::book_1,
                                                  BookId::book_2, BookId::book_3};

[[nodiscard]] constexpr OrderId id(std::uint64_t value) noexcept { return OrderId{value}; }

}  // namespace

int main() {
    test::Suite suite;

    suite.run("spsc ring is FIFO and reports full/empty", [&] {
        SpscRing<int> ring{4};
        int out{0};
        suite.expect(!ring.pop(out), "empty ring returned a value");
        for (int value{0}; value < 4; ++value) {
            suite.expect(ring.push(value), "push into non-full ring failed");
        }
        suite.expect(!ring.push(99), "push into full ring succeeded");
        for (int value{0}; value < 4; ++value) {
            suite.expect(ring.pop(out), "pop from non-empty ring failed");
            suite.equal(out, value, "ring violated FIFO order");
        }
        suite.expect(!ring.pop(out), "drained ring still returned a value");
    });

    suite.run("merged stream delivers every book's events, tagged, with no drops", [&] {
        constexpr int trades_per_book{500};
        // Ring per book holds >= trades_per_book, so even if the consumer never runs
        // no event is dropped; the assertion below then requires a zero drop count.
        MergedEventStream stream{trades_per_book};
        FourBookConfig config;
        for (std::size_t i{0}; i < book_count; ++i) {
            config.books[i] = BookConfig{2 * trades_per_book + 10, &stream.sink(static_cast<BookId>(i))};
        }
        FourBookEngine engine{config};

        std::array<std::atomic<int>, book_count> executions{};
        std::atomic<bool> done{false};
        const auto consume = [&](const RoutedEvent& event) {
            if (event.kind == RoutedEvent::Kind::execution) {
                executions[index_of(event.book)].fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread consumer{[&] {
            while (!done.load(std::memory_order_acquire)) {
                static_cast<void>(stream.drain(consume));
            }
        }};

        std::vector<std::thread> producers;
        producers.reserve(book_count);
        for (std::size_t book_index{0}; book_index < book_count; ++book_index) {
            producers.emplace_back([&, book_index] {
                const BookId book{all_books[book_index]};
                for (int k{0}; k < trades_per_book; ++k) {
                    const auto maker{id(static_cast<std::uint64_t>(2 * k + 1))};
                    const auto taker{id(static_cast<std::uint64_t>(2 * k + 2))};
                    static_cast<void>(engine.enqueue(book, LimitOrder{maker, Side::sell, trade_price, 1}));
                    static_cast<void>(engine.enqueue(book, LimitOrder{taker, Side::buy, trade_price, 1}));
                }
            });
        }
        for (auto& producer : producers) producer.join();

        engine.shutdown();                       // drains and joins workers; all events produced
        done.store(true, std::memory_order_release);
        consumer.join();
        static_cast<void>(stream.drain(consume));  // final sweep (single consumer now)

        int total_dropped{0};
        for (const BookId book : all_books) {
            suite.equal(executions[index_of(book)].load(), trades_per_book,
                        "a book delivered the wrong number of tagged executions");
            total_dropped += static_cast<int>(stream.dropped(book));
        }
        suite.equal(total_dropped, 0, "a large-enough ring still dropped events");
    });

    suite.run("ring overflow drops are counted, never silently lost", [&] {
        constexpr int trades{200};
        MergedEventStream stream{2};  // deliberately tiny; the consumer never runs during the run
        FourBookConfig config;
        config.books[0] = BookConfig{2 * trades + 10, &stream.sink(BookId::book_0)};
        for (std::size_t i{1}; i < book_count; ++i) config.books[i] = BookConfig{16, nullptr};
        FourBookEngine engine{config};

        for (int k{0}; k < trades; ++k) {
            static_cast<void>(engine.enqueue(BookId::book_0,
                LimitOrder{id(static_cast<std::uint64_t>(2 * k + 1)), Side::sell, trade_price, 1}));
            static_cast<void>(engine.enqueue(BookId::book_0,
                LimitOrder{id(static_cast<std::uint64_t>(2 * k + 2)), Side::buy, trade_price, 1}));
        }
        engine.shutdown();

        int delivered{0};
        static_cast<void>(stream.drain([&](const RoutedEvent& event) {
            if (event.kind == RoutedEvent::Kind::execution) ++delivered;
        }));
        const int dropped{static_cast<int>(stream.dropped(BookId::book_0))};
        suite.expect(dropped > 0, "a tiny ring should have dropped some events");
        suite.equal(delivered + dropped, trades,
                    "delivered + dropped must account for every produced event");
    });

    return suite.finish();
}
