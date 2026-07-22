#include "orderbook/events.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/order_index.hpp"
#include "orderbook/order_pool.hpp"
#include "orderbook/types.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace {

using orderbook::BookConfig;
using orderbook::LimitOrder;
using orderbook::MarketOrder;
using orderbook::OrderBook;
using orderbook::OrderId;
using orderbook::Price;
using orderbook::Quantity;
using orderbook::RecordingEventSink;
using orderbook::Side;
using orderbook::Status;
using orderbook::TimeInForce;

constexpr Price price_99{990'000};
constexpr Price price_100{1'000'000};
constexpr Price price_101{1'010'000};
constexpr Price price_102{1'020'000};

[[nodiscard]] constexpr OrderId id(std::uint64_t value) noexcept {
    return OrderId{value};
}

void require_valid(const OrderBook& book) {
    const auto validation{book.validate()};
    if (!validation.valid) {
        throw test::Failure{"book invariant failure: " + validation.message};
    }
}

}  // namespace

int main() {
    test::Suite suite;

    suite.run("checked decimal price conversion", [&] {
        const auto converted{orderbook::price_from_double(99.995)};
        suite.expect(converted.has_value(), "valid decimal price was rejected");
        suite.equal(converted->ticks(), std::int64_t{999'950}, "price rounded to wrong tick");
        suite.expect(std::abs(orderbook::price_to_double(*converted) - 99.995) < 1e-12,
                     "price did not round-trip");
        suite.expect(!orderbook::price_from_double(0.0).has_value(), "zero price accepted");
        suite.expect(!orderbook::price_from_double(-1.0).has_value(), "negative price accepted");
        suite.expect(!orderbook::price_from_double(
                         std::numeric_limits<double>::quiet_NaN()).has_value(),
                     "NaN price accepted");
        suite.expect(!orderbook::price_from_double(
                         std::numeric_limits<double>::infinity()).has_value(),
                     "infinite price accepted");
    });

    suite.run("order pool is bounded and reuses handles", [&] {
        orderbook::OrderPool pool{2};
        const auto first{pool.allocate(id(1), Side::buy, price_99, 10)};
        const auto second{pool.allocate(id(2), Side::sell, price_101, 20)};
        suite.expect(first.has_value() && second.has_value(), "pool rejected valid allocations");
        suite.expect(!pool.allocate(id(3), Side::buy, price_99, 1).has_value(),
                     "pool exceeded configured capacity");
        pool.release(*first);
        const auto reused{pool.allocate(id(3), Side::buy, price_99, 1)};
        suite.equal(reused, first, "freelist did not reuse released handle");
        suite.equal(pool.size(), std::size_t{2}, "pool size drifted after reuse");
    });

    suite.run("fixed order index survives deletion churn", [&] {
        orderbook::OrderIndex index{8};
        for (std::uint64_t value{1}; value <= 8; ++value) {
            suite.expect(index.insert(id(value), static_cast<orderbook::OrderHandle>(value - 1)),
                         "index insertion failed before capacity");
        }
        suite.expect(!index.insert(id(9), 9), "index exceeded configured active capacity");
        for (std::uint64_t value{1}; value <= 8; value += 2) {
            suite.expect(index.erase(id(value)), "index erase failed");
        }
        for (std::uint64_t value{9}; value <= 12; ++value) {
            suite.expect(index.insert(id(value), static_cast<orderbook::OrderHandle>(value)),
                         "index failed to reuse tombstoned storage");
        }
        for (std::uint64_t value{2}; value <= 8; value += 2) {
            suite.expect(index.contains(id(value)), "surviving index entry was lost");
        }
        suite.equal(index.size(), std::size_t{8}, "index size is incorrect after churn");
    });

    suite.run("noncrossing limits rest and expose top of book", [&] {
        OrderBook book;
        suite.expect(book.submit(LimitOrder{id(1), Side::buy, price_99, 10}).accepted(),
                     "buy limit rejected");
        suite.expect(book.submit(LimitOrder{id(2), Side::sell, price_101, 5}).accepted(),
                     "sell limit rejected");
        suite.equal(book.stats().active_orders, std::size_t{2}, "active count is wrong");
        suite.equal(book.best_bid()->price, price_99, "best bid is wrong");
        suite.equal(book.best_bid()->total_quantity, std::uint64_t{10},
                    "best bid aggregate is wrong");
        suite.equal(book.best_ask()->price, price_101, "best ask is wrong");
        require_valid(book);
    });

    suite.run("crossing limit executes at maker price", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{100, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 10}));
        const auto result{book.submit(LimitOrder{id(2), Side::buy, price_101, 10})};
        suite.equal(result.status, Status::accepted, "crossing order rejected");
        suite.equal(result.executed_quantity, Quantity{10}, "wrong executed quantity");
        suite.equal(result.remaining_quantity, Quantity{0}, "filled order had remainder");
        suite.equal(events.executions().size(), std::size_t{1}, "wrong event count");
        suite.equal(events.executions()[0].maker_id, id(1), "wrong maker ID");
        suite.equal(events.executions()[0].taker_id, id(2), "wrong taker ID");
        suite.equal(events.executions()[0].price, price_100, "did not execute at maker price");
        suite.equal(book.stats().active_orders, std::size_t{0}, "filled orders remained active");
        require_valid(book);
    });

    suite.run("partial maker and taker fills update state", [&] {
        OrderBook book;
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 10}));
        const auto first{book.submit(LimitOrder{id(2), Side::buy, price_100, 3})};
        suite.equal(first.executed_quantity, Quantity{3}, "first execution quantity is wrong");
        suite.equal(book.find(id(1))->remaining_quantity, Quantity{7}, "maker remainder is wrong");
        suite.expect(!book.find(id(2)).has_value(), "fully filled taker rested");
        const auto second{book.submit(LimitOrder{id(3), Side::buy, price_100, 9})};
        suite.equal(second.executed_quantity, Quantity{7}, "second executed quantity is wrong");
        suite.equal(second.remaining_quantity, Quantity{2}, "second remainder is wrong");
        suite.equal(book.find(id(3))->remaining_quantity, Quantity{2}, "taker remainder not resting");
        require_valid(book);
    });

    suite.run("best price then FIFO determine execution order", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{100, &events}};
        static_cast<void>(book.submit(LimitOrder{id(3), Side::sell, price_101, 4}));
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 5}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::sell, price_100, 6}));
        static_cast<void>(book.submit(LimitOrder{id(4), Side::buy, price_102, 13}));
        suite.equal(events.executions().size(), std::size_t{3}, "wrong sweep execution count");
        suite.equal(events.executions()[0].maker_id, id(1), "first maker violated priority");
        suite.equal(events.executions()[1].maker_id, id(2), "second maker violated FIFO");
        suite.equal(events.executions()[2].maker_id, id(3), "third maker used wrong level");
        suite.equal(book.find(id(3))->remaining_quantity, Quantity{2}, "last maker remainder wrong");
        require_valid(book);
    });

    suite.run("sell aggressor consumes highest bids first", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{100, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::buy, price_99, 5}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::buy, price_100, 5}));
        static_cast<void>(book.submit(LimitOrder{id(3), Side::sell, price_99, 7}));
        suite.equal(events.executions().size(), std::size_t{2}, "wrong bid sweep count");
        suite.equal(events.executions()[0].price, price_100, "lower bid executed first");
        suite.equal(events.executions()[1].price, price_99, "second bid price is wrong");
        suite.equal(book.find(id(1))->remaining_quantity, Quantity{3}, "lower bid remainder wrong");
        require_valid(book);
    });

    suite.run("market order reports unfilled quantity and never rests", [&] {
        OrderBook book;
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 4}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::sell, price_101, 3}));
        const auto result{book.submit(MarketOrder{id(3), Side::buy, 10})};
        suite.equal(result.executed_quantity, Quantity{7}, "market executed quantity wrong");
        suite.equal(result.remaining_quantity, Quantity{3}, "market remainder wrong");
        suite.expect(!book.find(id(3)).has_value(), "market order rested");
        suite.equal(book.stats().active_orders, std::size_t{0}, "makers remained after sweep");
        require_valid(book);
    });

    suite.run("IOC limit cancels its noncrossing remainder", [&] {
        OrderBook book;
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 3}));
        const auto result{book.submit(
            LimitOrder{id(2), Side::buy, price_100, 5, TimeInForce::ioc})};
        suite.equal(result.executed_quantity, Quantity{3}, "IOC fill quantity wrong");
        suite.equal(result.remaining_quantity, Quantity{2}, "IOC cancelled remainder wrong");
        suite.expect(!book.find(id(2)).has_value(), "IOC remainder rested");
        require_valid(book);
    });

    suite.run("FOK rejects atomically or fills completely", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{100, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 3}));
        const auto rejected{book.submit(
            LimitOrder{id(2), Side::buy, price_100, 5, TimeInForce::fok})};
        suite.equal(rejected.status, Status::would_not_fully_fill, "insufficient FOK accepted");
        suite.equal(book.find(id(1))->remaining_quantity, Quantity{3},
                    "rejected FOK mutated maker");
        suite.equal(events.executions().size(), std::size_t{0}, "rejected FOK emitted execution");
        static_cast<void>(book.submit(LimitOrder{id(3), Side::sell, price_100, 2}));
        const auto accepted{book.submit(
            LimitOrder{id(4), Side::buy, price_100, 5, TimeInForce::fok})};
        suite.equal(accepted.executed_quantity, Quantity{5}, "fillable FOK did not fully execute");
        suite.equal(accepted.remaining_quantity, Quantity{0}, "filled FOK retained remainder");
        require_valid(book);
    });

    suite.run("middle cancellation preserves FIFO and aggregates", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{100, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 5}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::sell, price_100, 6}));
        static_cast<void>(book.submit(LimitOrder{id(3), Side::sell, price_100, 7}));
        suite.equal(book.cancel(id(2)), Status::accepted, "middle cancel failed");
        suite.equal(book.level(Side::sell, price_100)->order_count, std::uint32_t{2},
                    "level count did not update");
        suite.equal(book.level(Side::sell, price_100)->total_quantity, std::uint64_t{12},
                    "level aggregate did not update");
        static_cast<void>(book.submit(MarketOrder{id(4), Side::buy, 8}));
        suite.equal(events.executions()[0].maker_id, id(1), "head no longer first after cancel");
        suite.equal(events.executions()[1].maker_id, id(3), "tail did not follow cancelled middle");
        suite.equal(book.find(id(3))->remaining_quantity, Quantity{4}, "tail remainder wrong");
        require_valid(book);
    });

    suite.run("head tail and last cancellation repair level", [&] {
        OrderBook book;
        static_cast<void>(book.submit(LimitOrder{id(1), Side::buy, price_100, 1}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::buy, price_100, 2}));
        static_cast<void>(book.submit(LimitOrder{id(3), Side::buy, price_100, 3}));
        suite.equal(book.cancel(id(1)), Status::accepted, "head cancel failed");
        suite.equal(book.cancel(id(3)), Status::accepted, "tail cancel failed");
        suite.equal(book.level(Side::buy, price_100)->total_quantity, std::uint64_t{2},
                    "middle aggregate wrong");
        suite.equal(book.cancel(id(2)), Status::accepted, "last cancel failed");
        suite.expect(!book.best_bid().has_value(), "empty bid level remained");
        require_valid(book);
    });

    suite.run("invalid duplicate and missing requests are observable", [&] {
        RecordingEventSink events;
        OrderBook book{BookConfig{10, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::buy, price_99, 1}));
        suite.equal(book.submit(LimitOrder{id(1), Side::sell, price_101, 1}).status,
                    Status::duplicate_order_id, "duplicate ID accepted");
        suite.equal(book.submit(LimitOrder{id(0), Side::buy, price_99, 1}).status,
                    Status::invalid_order_id, "zero ID accepted");
        suite.equal(book.submit(LimitOrder{id(2), Side::buy, Price{0}, 1}).status,
                    Status::invalid_price, "zero price accepted");
        suite.equal(book.submit(MarketOrder{id(3), Side::buy, 0}).status,
                    Status::invalid_quantity, "zero quantity accepted");
        suite.equal(book.cancel(id(99)), Status::not_found, "missing cancellation accepted");
        suite.equal(events.rejections().size(), std::size_t{5}, "rejections were not emitted");
        suite.equal(book.stats().active_orders, std::size_t{1}, "rejection mutated book");
        suite.equal(book.cancel(id(1)), Status::accepted, "valid cancellation failed");
        suite.expect(book.submit(LimitOrder{id(1), Side::buy, price_99, 1}).accepted(),
                     "ID was not reusable after cancellation");
        require_valid(book);
    });

    suite.run("hard capacity rejects atomically and reuses space", [&] {
        OrderBook book{BookConfig{2, nullptr}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::buy, price_99, 1}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::buy, price_99, 1}));
        const auto rejected{book.submit(LimitOrder{id(3), Side::sell, price_99, 1})};
        suite.equal(rejected.status, Status::capacity_exceeded,
                    "full book accepted a GTC order before matching");
        suite.equal(book.stats().active_orders, std::size_t{2}, "capacity rejection mutated book");
        static_cast<void>(book.cancel(id(1)));
        suite.expect(book.submit(LimitOrder{id(3), Side::buy, price_99, 1}).accepted(),
                     "released capacity was not reusable");
        require_valid(book);
    });

    suite.run("bounded recording sink reports dropped events", [&] {
        RecordingEventSink events{1, 1};
        OrderBook book{BookConfig{10, &events}};
        static_cast<void>(book.submit(LimitOrder{id(1), Side::sell, price_100, 1}));
        static_cast<void>(book.submit(LimitOrder{id(2), Side::sell, price_100, 1}));
        static_cast<void>(book.submit(MarketOrder{id(3), Side::buy, 2}));
        static_cast<void>(book.cancel(id(999)));
        static_cast<void>(book.cancel(id(998)));
        suite.equal(events.executions().size(), std::size_t{1}, "execution capacity ignored");
        suite.equal(events.dropped_execution_count(), std::size_t{1}, "dropped execution uncounted");
        suite.equal(events.rejections().size(), std::size_t{1}, "rejection capacity ignored");
        suite.equal(events.dropped_rejection_count(), std::size_t{1}, "dropped rejection uncounted");
        require_valid(book);
    });

    suite.run("deterministic mixed-operation stress preserves invariants", [&] {
        OrderBook book{BookConfig{64, nullptr}};
        std::uint64_t state{0x4d595df4d0f33173ULL};
        auto next_random = [&] {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state;
        };

        for (std::size_t step{0}; step < 5'000; ++step) {
            const std::uint64_t random{next_random()};
            const OrderId order_id{id((random % 96) + 1)};
            const Side side{(random & 1U) == 0 ? Side::buy : Side::sell};
            const Price price{990'000 + static_cast<std::int64_t>((random >> 8U) % 20'001)};
            const Quantity quantity{static_cast<Quantity>(((random >> 24U) % 20) + 1)};
            switch ((random >> 32U) % 5) {
                case 0:
                case 1:
                    static_cast<void>(book.submit(
                        LimitOrder{order_id, side, price, quantity}));
                    break;
                case 2:
                    static_cast<void>(book.submit(
                        LimitOrder{order_id, side, price, quantity, TimeInForce::ioc}));
                    break;
                case 3:
                    static_cast<void>(book.submit(MarketOrder{order_id, side, quantity}));
                    break;
                case 4:
                    static_cast<void>(book.cancel(order_id));
                    break;
                default:
                    throw test::Failure{"unreachable operation selector"};
            }
            require_valid(book);
        }
    });

    return suite.finish();
}
