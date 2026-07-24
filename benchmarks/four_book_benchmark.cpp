// Multibook coordination benchmark for FourBookEngine (plan Phase 2). This measures
// the COORDINATION layer — routing, per-book queueing, four worker threads, and the
// std::future completion path — NOT the direct matching core. Do not compare its
// numbers with the single-book orderbook_benchmark; they measure different layers.
//
// The workload is deliberately order-independent: every add is a non-crossing buy
// and every cancel targets one of the same producer's own still-resting orders.
// Regardless of how the four workers interleave, no request is ever rejected and
// the per-book final state and checksum are deterministic, so a nonzero rejection
// count or a changed checksum flags a real defect rather than benign nondeterminism.
#include "orderbook/events.hpp"
#include "orderbook/four_book_engine.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace orderbook;
using Clock = std::chrono::steady_clock;

constexpr Price rest_price{990'000};  // all adds are buys here; nothing ever crosses

struct RoutedOp {
    BookId book;
    OrderRequest request;
};

enum class Shape { balanced, skewed, independent, single };

// Routes an add by its sequence number so the distribution is independent of the
// add/cancel cadence (cancels are routed back to their add's book separately).
[[nodiscard]] BookId route(Shape shape, std::size_t producer, std::uint64_t add_index) {
    const auto pick = [](std::uint64_t v) { return static_cast<BookId>(v % book_count); };
    switch (shape) {
        case Shape::balanced:
            return pick(add_index);  // even spread across all four books
        case Shape::skewed:
            // ~80% to the hot book_0, the rest spread across all books.
            return (add_index % 5 == 0) ? pick(add_index) : BookId::book_0;
        case Shape::independent:
            return pick(producer);  // producer p owns book p (no queue contention)
        case Shape::single:
            return BookId::book_0;
    }
    return BookId::book_0;
}

// Deterministic per-producer stream. ~1 in 4 ops is a cancel of one of this
// producer's own earlier adds (routed back to the book that add went to).
[[nodiscard]] std::vector<RoutedOp> make_stream(
    Shape shape, std::size_t producer, std::size_t ops) {
    std::vector<RoutedOp> stream;
    stream.reserve(ops);
    std::vector<std::pair<OrderId, BookId>> active;
    const std::uint64_t id_base{static_cast<std::uint64_t>(producer) * (ops + 1) + 1};
    std::uint64_t add_index{0};
    for (std::size_t k{0}; k < ops; ++k) {
        if (k % 4 == 3 && !active.empty()) {
            const auto [id, book] = active.back();
            active.pop_back();
            stream.push_back({book, CancelOrder{id}});
        } else {
            const BookId book{route(shape, producer, add_index)};
            const OrderId id{id_base + add_index};
            ++add_index;
            stream.push_back({book, LimitOrder{id, Side::buy, rest_price, 10}});
            active.emplace_back(id, book);
        }
    }
    return stream;
}

struct ThroughputResult {
    double seconds{0.0};
    std::uint64_t total_ops{0};
    std::uint64_t checksum{0};
    std::uint64_t rejections{0};
    std::array<std::uint64_t, book_count> per_book_ops{};
    bool valid{true};
};

[[nodiscard]] std::uint64_t checksum_of(const SubmitResult& r) noexcept {
    return static_cast<std::uint64_t>(r.executed_quantity) + r.remaining_quantity +
           r.execution_count + static_cast<std::uint8_t>(r.status);
}

[[nodiscard]] std::size_t per_book_capacity(const std::vector<std::vector<RoutedOp>>& streams) {
    // Upper bound on peak active per book: every op could be an add to one book.
    std::size_t total{0};
    for (const auto& s : streams) total += s.size();
    return total + book_count;  // slack
}

[[nodiscard]] ThroughputResult run_throughput(const std::vector<std::vector<RoutedOp>>& streams) {
    const std::size_t capacity{per_book_capacity(streams)};
    std::array<NullEventSink, book_count> sinks;
    FourBookConfig config;
    for (std::size_t i{0}; i < book_count; ++i) config.books[i] = BookConfig{capacity, &sinks[i]};
    FourBookEngine engine{config};

    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> checksum{0};
    std::atomic<std::uint64_t> rejections{0};
    ThroughputResult result;
    for (const auto& s : streams)
        for (const auto& op : s)
            ++result.per_book_ops[static_cast<std::size_t>(op.book)];

    std::vector<std::thread> producers;
    producers.reserve(streams.size());
    for (const auto& stream : streams) {
        producers.emplace_back([&, &stream = stream] {
            std::vector<std::future<SubmitResult>> futures;
            futures.reserve(stream.size());
            while (!go.load(std::memory_order_acquire)) { /* start barrier */ }
            for (const auto& op : stream) futures.push_back(engine.enqueue(op.book, op.request));
            std::uint64_t local_sum{0};
            std::uint64_t local_rej{0};
            for (auto& f : futures) {
                const SubmitResult r{f.get()};
                local_sum += checksum_of(r);
                local_rej += r.status != Status::accepted ? 1 : 0;
            }
            checksum.fetch_add(local_sum, std::memory_order_relaxed);
            rejections.fetch_add(local_rej, std::memory_order_relaxed);
        });
    }

    const auto start{Clock::now()};
    go.store(true, std::memory_order_release);
    for (auto& p : producers) p.join();
    const auto end{Clock::now()};

    engine.shutdown();
    for (std::size_t i{0}; i < book_count; ++i)
        result.valid = result.valid &&
            engine.book_after_shutdown(static_cast<BookId>(i)).validate().valid;

    result.seconds = std::chrono::duration<double>(end - start).count();
    for (const auto& s : streams) result.total_ops += s.size();
    result.checksum = checksum.load();
    result.rejections = rejections.load();
    return result;
}

// Enqueue-to-completion latency under sustained load with a bounded in-flight
// window: each producer keeps at most `window` requests outstanding, retiring the
// oldest (and timing it) before enqueuing the next. This reflects per-request
// response time at a realistic queue depth rather than whole-batch drain time.
[[nodiscard]] std::vector<std::uint64_t> run_latency(const std::vector<std::vector<RoutedOp>>& streams) {
    constexpr std::size_t window{32};
    const std::size_t capacity{per_book_capacity(streams)};
    std::array<NullEventSink, book_count> sinks;
    FourBookConfig config;
    for (std::size_t i{0}; i < book_count; ++i) config.books[i] = BookConfig{capacity, &sinks[i]};
    FourBookEngine engine{config};

    std::atomic<bool> go{false};
    std::vector<std::vector<std::uint64_t>> per_producer(streams.size());
    std::vector<std::thread> producers;
    producers.reserve(streams.size());
    for (std::size_t p{0}; p < streams.size(); ++p) {
        producers.emplace_back([&, p] {
            const auto& stream{streams[p]};
            std::deque<std::future<SubmitResult>> inflight;
            std::deque<std::uint64_t> enqueued_at;
            auto& out{per_producer[p]};
            out.reserve(stream.size());
            const auto now_ns = [] {
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count());
            };
            const auto retire_one = [&] {
                static_cast<void>(inflight.front().get());
                out.push_back(now_ns() - enqueued_at.front());
                inflight.pop_front();
                enqueued_at.pop_front();
            };
            while (!go.load(std::memory_order_acquire)) { /* start barrier */ }
            for (const auto& op : stream) {
                enqueued_at.push_back(now_ns());
                inflight.push_back(engine.enqueue(op.book, op.request));
                if (inflight.size() >= window) retire_one();
            }
            while (!inflight.empty()) retire_one();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    engine.shutdown();

    std::vector<std::uint64_t> all;
    for (auto& v : per_producer) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    return all;
}

[[nodiscard]] std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double frac) {
    if (sorted.empty()) return 0;
    const auto idx{static_cast<std::size_t>(frac * static_cast<double>(sorted.size() - 1))};
    return sorted[std::min(idx, sorted.size() - 1)];
}

void run_scenario(const char* name, Shape shape, std::size_t producer_count,
                  std::size_t throughput_ops, std::size_t latency_ops) {
    const auto build = [&](std::size_t ops) {
        std::vector<std::vector<RoutedOp>> streams;
        streams.reserve(producer_count);
        for (std::size_t p{0}; p < producer_count; ++p) streams.push_back(make_stream(shape, p, ops));
        return streams;
    };

    // Warmup on a throwaway engine.
    static_cast<void>(run_throughput(build(std::min<std::size_t>(throughput_ops, 5'000))));

    const ThroughputResult t{run_throughput(build(throughput_ops))};
    const std::vector<std::uint64_t> lat{run_latency(build(latency_ops))};

    std::cout << "  " << std::left << std::setw(13) << name
              << "producers=" << producer_count
              << " ops=" << t.total_ops
              << " | " << std::right << std::setw(11) << std::fixed << std::setprecision(0)
              << static_cast<double>(t.total_ops) / t.seconds << " ops/s"
              << " | latency(32 in-flight) p50=" << percentile(lat, 0.50) << "ns"
              << " p95=" << percentile(lat, 0.95) << "ns"
              << " p99=" << percentile(lat, 0.99) << "ns"
              << " | rej=" << t.rejections
              << " valid=" << (t.valid ? "yes" : "NO")
              << " checksum=" << t.checksum << '\n';

    std::cout << "                per-book ops/s:";
    for (std::size_t i{0}; i < book_count; ++i)
        std::cout << ' ' << std::setprecision(0)
                  << static_cast<double>(t.per_book_ops[i]) / t.seconds;
    std::cout << '\n';
}

}  // namespace

int main() {
    constexpr std::size_t throughput_ops{50'000};
    constexpr std::size_t latency_ops{10'000};

    std::cout << "FourBookEngine coordination benchmark\n"
              << "  implementation: per-book mutex + condition_variable queue, one worker\n"
              << "                  thread per book, std::future completions\n"
              << "  hardware_concurrency: " << std::thread::hardware_concurrency() << " logical cores\n"
              << "  workload: non-crossing buy adds + own cancels (order-independent)\n"
              << "  note: coordination-layer throughput incl. future overhead; NOT comparable\n"
              << "        to the single-book direct-core benchmark. Latency is enqueue->\n"
              << "        completion at a bounded 32-request in-flight window per producer.\n\n";

    run_scenario("single", Shape::single, 1, throughput_ops, latency_ops);
    run_scenario("independent", Shape::independent, book_count, throughput_ops, latency_ops);
    run_scenario("balanced", Shape::balanced, book_count, throughput_ops, latency_ops);
    run_scenario("skewed", Shape::skewed, book_count, throughput_ops, latency_ops);
    return 0;
}
