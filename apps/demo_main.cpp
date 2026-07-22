#include "orderbook/events.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

#include <iostream>

int main() {
    orderbook::PrintingEventSink output{std::cout};
    orderbook::OrderBook book{orderbook::BookConfig{1'000, &output}};

    static_cast<void>(book.submit(orderbook::LimitOrder{
        orderbook::OrderId{1}, orderbook::Side::sell, orderbook::Price{1'000'000}, 10}));
    static_cast<void>(book.submit(orderbook::LimitOrder{
        orderbook::OrderId{2}, orderbook::Side::sell, orderbook::Price{1'002'500}, 8}));
    static_cast<void>(book.submit(orderbook::LimitOrder{
        orderbook::OrderId{3}, orderbook::Side::buy, orderbook::Price{997'500}, 12}));

    const auto aggressive_result{book.submit(orderbook::LimitOrder{
        orderbook::OrderId{4},
        orderbook::Side::buy,
        orderbook::Price{1'002'500},
        14,
        orderbook::TimeInForce::ioc})};
    static_cast<void>(book.cancel(orderbook::OrderId{3}));

    std::cout << "RESULT executed=" << aggressive_result.executed_quantity
              << " cancelled_remainder=" << aggressive_result.remaining_quantity << '\n';
    if (const auto best_ask{book.best_ask()}; best_ask.has_value()) {
        std::cout << "BEST_ASK price=" << orderbook::price_to_double(best_ask->price)
                  << " quantity=" << best_ask->total_quantity
                  << " orders=" << best_ask->order_count << '\n';
    }
    const auto validation{book.validate()};
    if (!validation.valid) {
        std::cerr << "INVARIANT_FAILURE " << validation.message << '\n';
        return 1;
    }
    return output.failed() ? 1 : 0;
}
