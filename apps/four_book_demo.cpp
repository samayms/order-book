// Runnable example of FourBookEngine: four independent books matched in parallel
// by four worker threads. Each producer thread scripts the same small scenario on
// its own book, reusing the same OrderIds in every book to show that identity is
// per-book ((BookId, OrderId)). Events are recorded per book and printed after
// shutdown, when the workers have stopped and the books can be read safely.
#include "orderbook/events.hpp"
#include "orderbook/four_book_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

#include <array>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace orderbook;

constexpr std::array<BookId, book_count> books{BookId::book_0, BookId::book_1,
                                              BookId::book_2, BookId::book_3};

// Scripts one book: rest liquidity on both sides, cross it with an IOC buy that
// trades against the two resting asks, then cancel the resting bid.
SubmitResult drive_one_book(FourBookEngine& engine, BookId book) {
    static_cast<void>(engine.enqueue(book, LimitOrder{OrderId{1}, Side::sell, Price{1'000'000}, 10}));
    static_cast<void>(engine.enqueue(book, LimitOrder{OrderId{2}, Side::sell, Price{1'002'500}, 8}));
    static_cast<void>(engine.enqueue(book, LimitOrder{OrderId{3}, Side::buy, Price{997'500}, 12}));
    auto crossing{engine.enqueue(
        book, LimitOrder{OrderId{4}, Side::buy, Price{1'002'500}, 14, TimeInForce::ioc})};
    static_cast<void>(engine.enqueue(book, CancelOrder{OrderId{3}}));
    return crossing.get();
}

}  // namespace

int main() {
    std::array<RecordingEventSink, book_count> sinks;
    FourBookConfig config;
    for (std::size_t index{0}; index < book_count; ++index) {
        config.books[index] = BookConfig{1'000, &sinks[index]};
    }

    FourBookEngine engine{config};
    std::cout << "FourBookEngine: 4 books, 4 workers, matching in parallel.\n"
              << "accepting=" << (engine.accepting() ? "yes" : "no") << "\n\n";

    // One producer thread per book; any producer could target any book, but giving
    // each its own keeps the demo output legible.
    std::array<SubmitResult, book_count> crossing_results{};
    std::vector<std::thread> producers;
    producers.reserve(book_count);
    for (std::size_t index{0}; index < book_count; ++index) {
        producers.emplace_back([&, index] {
            crossing_results[index] = drive_one_book(engine, books[index]);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    engine.shutdown();  // stop and drain all four workers before reading the books

    int exit_code{0};
    for (std::size_t index{0}; index < book_count; ++index) {
        const OrderBook& book{engine.book_after_shutdown(books[index])};
        const auto& sink{sinks[index]};
        std::cout << "book " << index << ": "
                  << "trades=" << sink.executions().size()
                  << " ioc_executed=" << crossing_results[index].executed_quantity
                  << " ioc_cancelled=" << crossing_results[index].remaining_quantity
                  << " active_orders=" << book.stats().active_orders;
        if (const auto best_ask{book.best_ask()}; best_ask.has_value()) {
            std::cout << " best_ask=" << price_to_double(best_ask->price)
                      << " (qty " << best_ask->total_quantity << ")";
        } else {
            std::cout << " best_ask=none";
        }
        std::cout << '\n';

        const auto validation{book.validate()};
        if (!validation.valid) {
            std::cerr << "INVARIANT_FAILURE book " << index << ": " << validation.message << '\n';
            exit_code = 1;
        }
    }
    return exit_code;
}
