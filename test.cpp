#include <iostream>
#include <cassert>
#include <string>
#include <sstream>
using namespace std;

// Redirect cout to capture FILL/PARTIAL output
struct OutputCapture {
    stringstream buffer;
    streambuf* old;
    void start() { old = cout.rdbuf(buffer.rdbuf()); }
    string stop() { cout.rdbuf(old); string s = buffer.str(); buffer.str(""); return s; }
};

// Pull in OrderBook implementation (no separate main)
#define TESTING
// We include main.cpp but skip its main()
// So we redefine main to _main_skip
#define main _main_skip
#include "main.cpp"
#undef main

static int tests_passed = 0;
static int tests_failed = 0;

void check(bool condition, const string& name) {
    if (condition) {
        cout << "  PASS: " << name << "\n";
        tests_passed++;
    } else {
        cout << "  FAIL: " << name << "\n";
        tests_failed++;
    }
}

// Count occurrences of a substring in a string
int count_substr(const string& haystack, const string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

// ─── Test Cases ───

void test_limit_no_match() {
    cout << "Test: Limit orders with no match rest on book\n";
    OrderBook book;
    OutputCapture cap;

    cap.start();
    book.add_limit_order("B1", true, 99.0, 10);   // buy @ 99
    book.add_limit_order("S1", false, 101.0, 5);   // sell @ 101
    string out = cap.stop();

    check(out.find("FILL") == string::npos, "no fills when prices don't cross");
    check(book.get_order_quantity("B1") == 10, "buy order rests with full qty");
    check(book.get_order_quantity("S1") == 5, "sell order rests with full qty");
    check(book.active_order_count() == 2, "two orders on book");
}

void test_limit_exact_match() {
    cout << "Test: Limit order exact fill\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 10);

    cap.start();
    book.add_limit_order("B1", true, 100.0, 10);
    string out = cap.stop();

    check(out.find("FILL: 10 @ 100") != string::npos, "fills 10 @ 100");
    check(book.get_order_quantity("S1") == -1, "maker fully filled and removed");
    check(book.get_order_quantity("B1") == -1, "taker fully filled and removed");
    check(book.active_order_count() == 0, "book is empty");
}

void test_limit_partial_fill_taker() {
    cout << "Test: Limit order partial fill — taker has leftover\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);

    cap.start();
    book.add_limit_order("B1", true, 100.0, 8);
    string out = cap.stop();

    check(out.find("FILL: 5 @ 100") != string::npos, "fills 5 @ 100");
    check(book.get_order_quantity("S1") == -1, "maker fully filled");
    check(book.get_order_quantity("B1") == 3, "taker rests with 3 remaining");
}

void test_limit_partial_fill_maker() {
    cout << "Test: Limit order partial fill — maker has leftover\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 10);

    cap.start();
    book.add_limit_order("B1", true, 100.0, 3);
    string out = cap.stop();

    check(out.find("FILL: 3 @ 100") != string::npos, "fills 3 @ 100");
    check(book.get_order_quantity("S1") == 7, "maker has 7 remaining");
    check(book.get_order_quantity("B1") == -1, "taker fully filled");
}

void test_price_improvement() {
    cout << "Test: Buy at higher price fills at lower resting ask\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);

    cap.start();
    book.add_limit_order("B1", true, 105.0, 5);  // willing to pay 105, gets 100
    string out = cap.stop();

    check(out.find("FILL: 5 @ 100") != string::npos, "fills at maker's price (100), not taker's (105)");
}

void test_price_time_priority() {
    cout << "Test: Price-time priority (FIFO at same level)\n";
    OrderBook book;
    OutputCapture cap;

    // Two sells at same price — S1 first, then S2
    book.add_limit_order("S1", false, 100.0, 5);
    book.add_limit_order("S2", false, 100.0, 5);

    cap.start();
    book.add_limit_order("B1", true, 100.0, 7);
    string out = cap.stop();

    // S1 should fill first (5), then S2 fills 2
    check(out.find("maker=S1") != string::npos, "S1 fills first (time priority)");
    check(book.get_order_quantity("S1") == -1, "S1 fully filled");
    check(book.get_order_quantity("S2") == 3, "S2 has 3 remaining");
}

void test_multi_level_fill() {
    cout << "Test: Fill across multiple price levels\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);
    book.add_limit_order("S2", false, 101.0, 5);
    book.add_limit_order("S3", false, 102.0, 5);

    cap.start();
    book.add_limit_order("B1", true, 102.0, 12);
    string out = cap.stop();

    check(count_substr(out, "FILL") == 3, "three fills across three levels");
    check(out.find("FILL: 5 @ 100") != string::npos, "fills 5 @ 100 first (best ask)");
    check(out.find("FILL: 5 @ 101") != string::npos, "fills 5 @ 101 second");
    check(out.find("FILL: 2 @ 102") != string::npos, "fills 2 @ 102 last");
    check(book.get_order_quantity("S3") == 3, "S3 has 3 remaining");
}

void test_cancel_order() {
    cout << "Test: Cancel order\n";
    OrderBook book;

    book.add_limit_order("B1", true, 99.0, 10);
    check(book.get_order_quantity("B1") == 10, "order on book before cancel");

    book.cancel_order("B1");
    check(book.get_order_quantity("B1") == -1, "order removed after cancel");
    check(book.active_order_count() == 0, "book empty after cancel");
}

void test_cancel_nonexistent() {
    cout << "Test: Cancel nonexistent order (no crash)\n";
    OrderBook book;
    book.cancel_order("DOESNOTEXIST");  // should not crash
    check(true, "no crash on cancel of nonexistent order");
}

void test_cancel_then_no_match() {
    cout << "Test: Cancelled order is skipped during matching\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);
    book.add_limit_order("S2", false, 100.0, 5);
    book.cancel_order("S1");

    cap.start();
    book.add_limit_order("B1", true, 100.0, 3);
    string out = cap.stop();

    check(out.find("maker=S2") != string::npos, "matches S2, not cancelled S1");
    check(out.find("maker=S1") == string::npos, "S1 is skipped");
}

void test_market_order_full_fill() {
    cout << "Test: Market order full fill\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 10);

    cap.start();
    book.add_market_order("M1", true, 10);
    string out = cap.stop();

    check(out.find("FILL: 10 @ 100") != string::npos, "market buy fills at best ask");
    check(out.find("PARTIAL") == string::npos, "no partial message");
    check(book.active_order_count() == 0, "book empty");
}

void test_market_order_partial() {
    cout << "Test: Market order partial fill (insufficient liquidity)\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);

    cap.start();
    book.add_market_order("M1", true, 8);
    string out = cap.stop();

    check(out.find("FILL: 5 @ 100") != string::npos, "fills what's available");
    check(out.find("PARTIAL") != string::npos, "reports partial fill");
    check(out.find("3 unfilled") != string::npos, "3 unfilled quantity");
}

void test_market_order_empty_book() {
    cout << "Test: Market order on empty book\n";
    OrderBook book;
    OutputCapture cap;

    cap.start();
    book.add_market_order("M1", true, 10);
    string out = cap.stop();

    check(out.find("FILL") == string::npos, "no fills on empty book");
    check(out.find("PARTIAL") != string::npos, "reports partial/unfilled");
}

void test_sell_side_matching() {
    cout << "Test: Sell limit matching against resting buys\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("B1", true, 100.0, 5);
    book.add_limit_order("B2", true, 99.0, 5);

    cap.start();
    book.add_limit_order("S1", false, 99.0, 7);
    string out = cap.stop();

    // Should match B1 first (highest bid = 100), then B2 (99)
    check(out.find("FILL: 5 @ 100") != string::npos, "fills against best bid (100) first");
    check(out.find("FILL: 2 @ 99") != string::npos, "fills 2 @ 99 second");
    check(book.get_order_quantity("B2") == 3, "B2 has 3 remaining");
}

void test_market_sell() {
    cout << "Test: Market sell order\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("B1", true, 100.0, 10);

    cap.start();
    book.add_market_order("M1", false, 5);
    string out = cap.stop();

    check(out.find("FILL: 5 @ 100") != string::npos, "market sell fills against best bid");
    check(book.get_order_quantity("B1") == 5, "B1 has 5 remaining");
}

void test_multiple_cancels_then_fill() {
    cout << "Test: Multiple cancels then fill reaches deep order\n";
    OrderBook book;
    OutputCapture cap;

    book.add_limit_order("S1", false, 100.0, 5);
    book.add_limit_order("S2", false, 100.0, 5);
    book.add_limit_order("S3", false, 100.0, 5);
    book.cancel_order("S1");
    book.cancel_order("S2");

    cap.start();
    book.add_limit_order("B1", true, 100.0, 3);
    string out = cap.stop();

    check(out.find("maker=S3") != string::npos, "skips cancelled S1/S2, matches S3");
    check(book.get_order_quantity("S3") == 2, "S3 has 2 remaining");
}

int main() {
    cout << "=== OrderBook Tests ===\n\n";

    test_limit_no_match();
    test_limit_exact_match();
    test_limit_partial_fill_taker();
    test_limit_partial_fill_maker();
    test_price_improvement();
    test_price_time_priority();
    test_multi_level_fill();
    test_cancel_order();
    test_cancel_nonexistent();
    test_cancel_then_no_match();
    test_market_order_full_fill();
    test_market_order_partial();
    test_market_order_empty_book();
    test_sell_side_matching();
    test_market_sell();
    test_multiple_cancels_then_fill();

    cout << "\n=== Results: " << tests_passed << " passed, "
         << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
