#include "orderbook/events.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class OperationType : std::uint8_t { limit_gtc, limit_ioc, market, cancel };

struct Operation {
    OperationType type{OperationType::limit_gtc};
    orderbook::OrderId id;
    orderbook::Side side{orderbook::Side::buy};
    orderbook::Price price;
    orderbook::Quantity quantity{0};
    orderbook::TimeInForce time_in_force{orderbook::TimeInForce::gtc};
};

struct BenchmarkConfig {
    std::size_t operation_count{350'000};
    std::size_t throughput_iterations{200};
    std::size_t depth{10'000};  // resting depth prefill
};

[[nodiscard]] std::vector<Operation> make_workload(std::size_t requested_operations) {
    constexpr std::size_t operations_per_group{7};
    const std::size_t group_count{
        std::max<std::size_t>(1, requested_operations / operations_per_group)};
    std::vector<Operation> operations;
    operations.reserve(group_count * operations_per_group);

    for (std::size_t group{0}; group < group_count; ++group) {
        const auto base_id{static_cast<std::uint64_t>(group * 10 + 1)};
        const auto price_offset{static_cast<std::int64_t>(group % 100)};
        const orderbook::Price ask{1'000'000 + price_offset};
        const orderbook::Price bid{990'000 - price_offset};
        operations.push_back({OperationType::limit_gtc, orderbook::OrderId{base_id},
                              orderbook::Side::sell, ask, 10});
        operations.push_back({OperationType::limit_gtc, orderbook::OrderId{base_id + 1},
                              orderbook::Side::sell, ask, 10});
        operations.push_back({OperationType::limit_gtc, orderbook::OrderId{base_id + 2},
                              orderbook::Side::buy, bid, 10});
        operations.push_back({OperationType::market, orderbook::OrderId{base_id + 3},
                              orderbook::Side::buy, {}, 7});
        operations.push_back({OperationType::limit_ioc, orderbook::OrderId{base_id + 4},
                              orderbook::Side::buy, ask, 8, orderbook::TimeInForce::ioc});
        operations.push_back({OperationType::cancel, orderbook::OrderId{base_id + 1},
                              orderbook::Side::buy, {}, 0, orderbook::TimeInForce::gtc});
        operations.push_back({OperationType::cancel, orderbook::OrderId{base_id + 2},
                              orderbook::Side::buy, {}, 0, orderbook::TimeInForce::gtc});
    }
    return operations;
}

[[nodiscard]] orderbook::SubmitResult apply(
    orderbook::OrderBook& book, const Operation& operation) {
    switch (operation.type) {
        case OperationType::limit_gtc:
            return book.submit(orderbook::LimitOrder{
                operation.id, operation.side, operation.price,
                operation.quantity, orderbook::TimeInForce::gtc});
        case OperationType::limit_ioc:
            return book.submit(orderbook::LimitOrder{
                operation.id, operation.side, operation.price,
                operation.quantity, orderbook::TimeInForce::ioc});
        case OperationType::market:
            return book.submit(orderbook::MarketOrder{
                operation.id, operation.side, operation.quantity});
        case OperationType::cancel:
            return orderbook::SubmitResult{book.cancel(operation.id), 0, 0, 0};
    }
    return orderbook::SubmitResult{orderbook::Status::engine_stopped, 0, 0, 0};
}

// `rejections` counts every non-accepted status; the deterministic workload is
// expected to produce zero, so a nonzero value flags a broken workload (e.g.
// duplicate_order_id from id reuse across throughput iterations).
[[nodiscard]] std::uint64_t run_workload(
    orderbook::OrderBook& book, const std::vector<Operation>& operations,
    std::uint64_t& rejections) {
    std::uint64_t checksum{0};
    for (const Operation& operation : operations) {
        const auto result{apply(book, operation)};
        checksum += result.executed_quantity;
        checksum += result.remaining_quantity;
        checksum += result.execution_count;
        checksum += static_cast<std::uint8_t>(result.status);
        rejections += result.status != orderbook::Status::accepted ? 1 : 0;
    }
    const auto stats{book.stats()};
    return checksum + stats.execution_count + stats.active_orders;
}

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted_values, double fraction) {
    if (sorted_values.empty()) return 0;
    const auto index{static_cast<std::size_t>(
        fraction * static_cast<double>(sorted_values.size() - 1))};
    return sorted_values[std::min(index, sorted_values.size() - 1)];
}

void report_latencies(
    std::string_view op_type_name,
    const std::vector<std::uint64_t>& latencies) {
    if (latencies.empty()) {
        std::cout << "    " << op_type_name << ": (no samples)\n";
        return;
    }
    auto sorted{latencies};
    std::sort(sorted.begin(), sorted.end());
    std::cout << "    " << op_type_name << ":\n"
              << "      p50:  " << percentile(sorted, 0.50) << " ns\n"
              << "      p95:  " << percentile(sorted, 0.95) << " ns\n"
              << "      p99:  " << percentile(sorted, 0.99) << " ns\n"
              << "      p99.9:" << percentile(sorted, 0.999) << " ns\n"
              << "      max:  " << sorted.back() << " ns\n";
}

[[nodiscard]] BenchmarkConfig parse_config(int argc, char** argv) {
    BenchmarkConfig config;
    for (int index{1}; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--operations" && index + 1 < argc) {
            config.operation_count = static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--iterations" && index + 1 < argc) {
            config.throughput_iterations =
                static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--depth" && index + 1 < argc) {
            config.depth = static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--help") {
            std::cout << "usage: orderbook_benchmark [--operations N] [--iterations N] [--depth N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument{"unknown benchmark argument"};
        }
    }
    if (config.throughput_iterations == 0) {
        throw std::invalid_argument{"throughput iterations must be positive"};
    }
    return config;
}

void prefill_book(orderbook::OrderBook& book, std::size_t depth) {
    // Rest `depth` GTC sells across 5,000 price levels starting at tick 1,100,000,
    // far above the workload's top ask (~1,000,099) so they are never matched or
    // cancelled by the measured operations. OrderIds start at 9,000,000 to avoid
    // colliding with workload ids.
    constexpr std::int64_t prefill_base_ticks{1'100'000};
    for (std::size_t i{0}; i < depth; ++i) {
        const orderbook::OrderId order_id{9'000'000 + static_cast<std::uint64_t>(i)};
        const orderbook::Price price{
            prefill_base_ticks + static_cast<std::int64_t>(i % 5'000)};
        const auto result{book.submit(orderbook::LimitOrder{
            order_id, orderbook::Side::sell, price, 1000, orderbook::TimeInForce::gtc})};
        if (result.status != orderbook::Status::accepted) {
            throw std::runtime_error{"prefill failed: order rejected"};
        }
    }
}

struct ClockOverhead {
    std::uint64_t average_ns{0};
    std::uint64_t median_ns{0};
};

// Measures the cost of one back-to-back Clock::now() pair, which bounds the
// timing overhead baked into every per-operation latency sample below.
[[nodiscard]] ClockOverhead measure_clock_overhead() {
    constexpr std::size_t iterations{100'000};
    std::vector<std::uint64_t> timings;
    timings.reserve(iterations);
    std::uint64_t total{0};
    for (std::size_t i{0}; i < iterations; ++i) {
        const auto start{Clock::now()};
        const auto end{Clock::now()};
        const auto delta{static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count())};
        timings.push_back(delta);
        total += delta;
    }
    std::sort(timings.begin(), timings.end());
    return ClockOverhead{total / iterations, timings[timings.size() / 2]};
}

void require_valid(const orderbook::OrderBook& book, std::string_view pass_name) {
    const auto validation{book.validate()};
    if (!validation.valid) {
        throw std::runtime_error{
            std::string{pass_name} + " invariant failure: " + validation.message};
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const BenchmarkConfig config{parse_config(argc, argv)};
        const auto workload{make_workload(config.operation_count)};
        orderbook::NullEventSink events;

        // Measure clock overhead first
        const ClockOverhead clock_overhead{measure_clock_overhead()};

        // Capacity must accommodate prefill + workload operations at any time
        const std::size_t required_capacity{config.depth + 7};

        // Warmup pass with prefill
        const auto warmup_workload{
            make_workload(std::min<std::size_t>(workload.size(), 14'000))};
        orderbook::OrderBook warmup_book{orderbook::BookConfig{required_capacity, &events}};
        prefill_book(warmup_book, config.depth);
        require_valid(warmup_book, "warmup_prefill");
        std::uint64_t warmup_rejections{0};
        const std::uint64_t warmup_checksum{
            run_workload(warmup_book, warmup_workload, warmup_rejections)};
        require_valid(warmup_book, "warmup");

        // Throughput pass with prefill
        orderbook::OrderBook throughput_book{orderbook::BookConfig{required_capacity, &events}};
        prefill_book(throughput_book, config.depth);
        require_valid(throughput_book, "throughput_prefill");
        const auto throughput_start{Clock::now()};
        std::uint64_t throughput_checksum{0};
        std::uint64_t throughput_rejections{0};
        for (std::size_t iteration{0}; iteration < config.throughput_iterations; ++iteration) {
            throughput_checksum +=
                run_workload(throughput_book, workload, throughput_rejections);
        }
        const auto throughput_end{Clock::now()};
        require_valid(throughput_book, "throughput");
        const double elapsed_seconds{
            std::chrono::duration<double>(throughput_end - throughput_start).count()};

        // Latency pass with prefill and per-operation tracking
        orderbook::OrderBook latency_book{orderbook::BookConfig{required_capacity, &events}};
        prefill_book(latency_book, config.depth);
        require_valid(latency_book, "latency_prefill");

        std::vector<std::uint64_t> latencies_limit_gtc;
        std::vector<std::uint64_t> latencies_limit_ioc;
        std::vector<std::uint64_t> latencies_market;
        std::vector<std::uint64_t> latencies_cancel;
        latencies_limit_gtc.reserve(workload.size());
        latencies_limit_ioc.reserve(workload.size());
        latencies_market.reserve(workload.size());
        latencies_cancel.reserve(workload.size());

        std::uint64_t latency_checksum{0};
        std::uint64_t latency_rejections{0};
        for (const Operation& operation : workload) {
            const auto start{Clock::now()};
            const auto result{apply(latency_book, operation)};
            const auto end{Clock::now()};
            const auto duration{static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count())};

            switch (operation.type) {
                case OperationType::limit_gtc:
                    latencies_limit_gtc.push_back(duration);
                    break;
                case OperationType::limit_ioc:
                    latencies_limit_ioc.push_back(duration);
                    break;
                case OperationType::market:
                    latencies_market.push_back(duration);
                    break;
                case OperationType::cancel:
                    latencies_cancel.push_back(duration);
                    break;
            }
            latency_checksum += result.executed_quantity;
            latency_checksum += result.remaining_quantity;
            latency_checksum += result.execution_count;
            latency_rejections += result.status != orderbook::Status::accepted ? 1 : 0;
        }
        require_valid(latency_book, "latency");

        const std::uint64_t total_rejections{
            warmup_rejections + throughput_rejections + latency_rejections};
        if (total_rejections != 0) {
            throw std::runtime_error{
                "workload produced " + std::to_string(total_rejections) +
                " rejected operations; expected zero"};
        }

#ifdef NDEBUG
        constexpr std::string_view build_type{"release"};
#else
        constexpr std::string_view build_type{"debug (not representative)"};
#endif

        const std::size_t measured_throughput_operations{
            workload.size() * config.throughput_iterations};

        std::cout << "Order book benchmark (realistic depth: " << config.depth << " resting orders)\n"
                  << "  build:       " << build_type << '\n'
                  << "  workload:    deterministic GTC adds, market/IOC matching, cancels\n"
                  << "  prefill:     " << config.depth << " resting sell orders @ 1,100,000+\n"
                  << "  operations:  " << measured_throughput_operations
                  << " (" << workload.size() << " x "
                  << config.throughput_iterations << ")\n"
                  << "  elapsed:     " << std::fixed << std::setprecision(6)
                  << elapsed_seconds << " s\n"
                  << "  throughput:  " << std::setprecision(0)
                  << static_cast<double>(measured_throughput_operations) / elapsed_seconds
                  << " ops/s\n"
                  << "  clock overhead: " << clock_overhead.average_ns << " ns avg, "
                  << clock_overhead.median_ns << " ns median (per Clock::now() pair)\n"
                  << "  rejections:  " << total_rejections << " (expected 0)\n"
                  << "  latency by operation type:\n";
        report_latencies("limit-gtc", latencies_limit_gtc);
        report_latencies("limit-ioc", latencies_limit_ioc);
        report_latencies("market", latencies_market);
        report_latencies("cancel", latencies_cancel);
        std::cout << "  checksum:    "
                  << warmup_checksum + throughput_checksum + latency_checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
