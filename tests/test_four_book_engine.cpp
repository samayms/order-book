#include "orderbook/events.hpp"
#include "orderbook/four_book_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"
#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace {

using orderbook::BookConfig;
using orderbook::BookId;
using orderbook::FourBookConfig;
using orderbook::FourBookEngine;
using orderbook::LimitOrder;
using orderbook::MarketOrder;
using orderbook::OrderId;
using orderbook::Price;
using orderbook::RecordingEventSink;
using orderbook::Side;
using orderbook::Status;
using orderbook::SubmitResult;

constexpr Price bid_price{990'000};
constexpr Price ask_price{1'000'000};
constexpr std::array<BookId, 4> all_books{BookId::book_0, BookId::book_1,
                                          BookId::book_2, BookId::book_3};

[[nodiscard]] constexpr OrderId id(std::uint64_t value) noexcept {
    return OrderId{value};
}

FourBookConfig uniform_config(std::size_t capacity) {
    FourBookConfig config;
    for (auto& book : config.books) {
        book = BookConfig{capacity, nullptr};
    }
    return config;
}

void require_valid(const FourBookEngine& engine, BookId book) {
    const auto validation{engine.book_after_shutdown(book).validate()};
    if (!validation.valid) {
        throw test::Failure{"book invariant failure: " + validation.message};
    }
}

}  // namespace

int main() {
    test::Suite suite;

    suite.run("routes each order only to its target book", [&] {
        FourBookEngine engine{uniform_config(100)};
        for (std::size_t index{0}; index < all_books.size(); ++index) {
            const auto order_id{id(static_cast<std::uint64_t>(index) + 1)};
            suite.expect(engine.enqueue(all_books[index],
                                        LimitOrder{order_id, Side::buy, bid_price, 5}).get().accepted(),
                         "routed order was rejected");
        }
        engine.shutdown();
        for (std::size_t index{0}; index < all_books.size(); ++index) {
            const auto order_id{id(static_cast<std::uint64_t>(index) + 1)};
            for (std::size_t other{0}; other < all_books.size(); ++other) {
                const bool present{
                    engine.book_after_shutdown(all_books[other]).find(order_id).has_value()};
                suite.expect(present == (other == index),
                             "order appeared in the wrong book or is missing");
            }
            require_valid(engine, all_books[index]);
        }
    });

    suite.run("the same OrderId is independent across books", [&] {
        FourBookEngine engine{uniform_config(100)};
        for (const BookId book : all_books) {
            suite.expect(engine.enqueue(book, LimitOrder{id(1), Side::buy, bid_price, 3}).get().accepted(),
                         "shared OrderId rejected in a book");
        }
        engine.shutdown();
        for (const BookId book : all_books) {
            suite.equal(engine.book_after_shutdown(book).stats().active_orders, std::size_t{1},
                        "book did not retain its own copy of the shared OrderId");
        }
    });

    suite.run("duplicate within a book does not touch the others", [&] {
        FourBookEngine engine{uniform_config(100)};
        suite.expect(engine.enqueue(BookId::book_0,
                                    LimitOrder{id(1), Side::buy, bid_price, 1}).get().accepted(),
                     "first order rejected");
        const auto duplicate{engine.enqueue(BookId::book_0,
                                            LimitOrder{id(1), Side::buy, bid_price, 1}).get()};
        suite.equal(duplicate.status, Status::duplicate_order_id, "duplicate not rejected");
        suite.expect(engine.enqueue(BookId::book_1,
                                    LimitOrder{id(1), Side::buy, bid_price, 1}).get().accepted(),
                     "same id in another book rejected");
        engine.shutdown();
        suite.equal(engine.book_after_shutdown(BookId::book_0).stats().active_orders, std::size_t{1},
                    "duplicate mutated book_0");
        suite.equal(engine.book_after_shutdown(BookId::book_1).stats().active_orders, std::size_t{1},
                    "book_1 disturbed by book_0 duplicate");
    });

    suite.run("cancel by (BookId, OrderId) affects only that book", [&] {
        FourBookEngine engine{uniform_config(100)};
        static_cast<void>(engine.enqueue(BookId::book_0, LimitOrder{id(1), Side::buy, bid_price, 1}));
        static_cast<void>(engine.enqueue(BookId::book_1, LimitOrder{id(1), Side::buy, bid_price, 1}));
        suite.equal(engine.enqueue(BookId::book_0, orderbook::CancelOrder{id(1)}).get().status,
                    Status::accepted, "cancel of resting order failed");
        engine.shutdown();
        suite.equal(engine.book_after_shutdown(BookId::book_0).stats().active_orders, std::size_t{0},
                    "cancel did not remove the order");
        suite.equal(engine.book_after_shutdown(BookId::book_1).stats().active_orders, std::size_t{1},
                    "cancel leaked into another book");
    });

    suite.run("execution sequences and events are per book", [&] {
        std::array<RecordingEventSink, 4> sinks;  // distinct sink per book (no sharing)
        FourBookConfig config;
        for (std::size_t index{0}; index < all_books.size(); ++index) {
            config.books[index] = BookConfig{100, &sinks[index]};
        }
        FourBookEngine engine{config};
        for (const BookId book : all_books) {
            static_cast<void>(engine.enqueue(book, LimitOrder{id(1), Side::sell, ask_price, 5}));
            suite.expect(engine.enqueue(book, LimitOrder{id(2), Side::buy, ask_price, 5}).get().accepted(),
                         "crossing buy rejected");
        }
        engine.shutdown();
        for (std::size_t index{0}; index < all_books.size(); ++index) {
            suite.equal(sinks[index].executions().size(), std::size_t{1},
                        "each book should record exactly one execution");
            suite.equal(sinks[index].executions()[0].sequence, orderbook::SequenceNumber{1},
                        "each book's execution sequence must start at 1 independently");
            require_valid(engine, all_books[index]);
        }
    });

    suite.run("concurrent producers, one per book, stay isolated", [&] {
        constexpr int per_book{200};
        FourBookEngine engine{uniform_config(per_book + 10)};
        std::atomic<bool> go{false};
        std::atomic<int> accepted{0};
        std::vector<std::thread> producers;
        producers.reserve(all_books.size());
        for (std::size_t book_index{0}; book_index < all_books.size(); ++book_index) {
            producers.emplace_back([&, book_index] {
                const BookId book{all_books[book_index]};
                std::vector<std::future<SubmitResult>> futures;
                futures.reserve(per_book);
                while (!go.load(std::memory_order_acquire)) {
                    // start barrier: spin until released, no real-time sleep
                }
                for (int offset{0}; offset < per_book; ++offset) {
                    const auto order_id{id(static_cast<std::uint64_t>(offset) + 1)};
                    futures.push_back(engine.enqueue(book,
                        LimitOrder{order_id, Side::buy, bid_price, 1}));
                }
                for (auto& future : futures) {
                    if (future.get().accepted()) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }
        engine.shutdown();
        suite.equal(accepted.load(std::memory_order_relaxed),
                    static_cast<int>(all_books.size()) * per_book,
                    "a concurrent request was lost or rejected");
        for (const BookId book : all_books) {
            suite.equal(engine.book_after_shutdown(book).stats().active_orders,
                        static_cast<std::size_t>(per_book),
                        "a book did not apply all of its own requests");
            require_valid(engine, book);
        }
    });

    suite.run("shutdown drains all books and is idempotent", [&] {
        constexpr int per_book{100};
        FourBookEngine engine{uniform_config(per_book + 10)};
        std::vector<std::future<SubmitResult>> futures;
        for (const BookId book : all_books) {
            for (int offset{0}; offset < per_book; ++offset) {
                futures.push_back(engine.enqueue(book,
                    LimitOrder{id(static_cast<std::uint64_t>(offset) + 1), Side::buy, bid_price, 1}));
            }
        }
        engine.shutdown();
        engine.shutdown();  // idempotent
        for (auto& future : futures) {
            suite.expect(future.get().accepted(), "accepted command lost during drain");
        }
        for (const BookId book : all_books) {
            suite.equal(engine.book_after_shutdown(book).stats().active_orders,
                        static_cast<std::size_t>(per_book), "a book dropped drained work");
        }
        suite.expect(!engine.accepting(), "engine still accepting after shutdown");
        suite.equal(engine.enqueue(BookId::book_0,
                                   LimitOrder{id(9999), Side::buy, bid_price, 1}).get().status,
                    Status::engine_stopped, "post-shutdown request admitted");
    });

    suite.run("submissions racing shutdown are all accounted for", [&] {
        constexpr int attempts{500};
        FourBookEngine engine{uniform_config(attempts + 10)};
        std::atomic<int> accepted{0};
        std::atomic<int> stopped{0};
        std::thread producer{[&] {
            std::vector<std::future<SubmitResult>> futures;
            futures.reserve(attempts);
            for (int offset{0}; offset < attempts; ++offset) {
                futures.push_back(engine.enqueue(BookId::book_2,
                    LimitOrder{id(static_cast<std::uint64_t>(offset) + 1), Side::buy, bid_price, 1}));
            }
            for (auto& future : futures) {
                const auto status{future.get().status};
                if (status == Status::accepted) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else if (status == Status::engine_stopped) {
                    stopped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }};
        engine.shutdown();
        producer.join();
        suite.equal(accepted.load() + stopped.load(), attempts,
                    "a racing request was neither accepted nor stopped");
        require_valid(engine, BookId::book_2);
    });

    suite.run("construct and destruct with no submitted work", [&] {
        { FourBookEngine engine{uniform_config(8)}; suite.expect(engine.accepting(), "fresh engine not accepting"); }
        suite.expect(true, "clean construction/destruction");
    });

    return suite.finish();
}
