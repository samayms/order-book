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
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class OperationType : std::uint8_t { limit, market, cancel };

struct Operation {
    OperationType type{OperationType::limit};
    orderbook::OrderId id;
    orderbook::Side side{orderbook::Side::buy};
    orderbook::Price price;
    orderbook::Quantity quantity{0};
    orderbook::TimeInForce time_in_force{orderbook::TimeInForce::gtc};
};

struct BenchmarkConfig {
    std::size_t operation_count{350'000};
    std::size_t throughput_iterations{200};
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
        operations.push_back({OperationType::limit, orderbook::OrderId{base_id},
                              orderbook::Side::sell, ask, 10});
        operations.push_back({OperationType::limit, orderbook::OrderId{base_id + 1},
                              orderbook::Side::sell, ask, 10});
        operations.push_back({OperationType::limit, orderbook::OrderId{base_id + 2},
                              orderbook::Side::buy, bid, 10});
        operations.push_back({OperationType::market, orderbook::OrderId{base_id + 3},
                              orderbook::Side::buy, {}, 7});
        operations.push_back({OperationType::limit, orderbook::OrderId{base_id + 4},
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
        case OperationType::limit:
            return book.submit(orderbook::LimitOrder{
                operation.id, operation.side, operation.price,
                operation.quantity, operation.time_in_force});
        case OperationType::market:
            return book.submit(orderbook::MarketOrder{
                operation.id, operation.side, operation.quantity});
        case OperationType::cancel:
            return orderbook::SubmitResult{book.cancel(operation.id), 0, 0, 0};
    }
    return orderbook::SubmitResult{orderbook::Status::engine_stopped, 0, 0, 0};
}

[[nodiscard]] std::uint64_t run_workload(
    orderbook::OrderBook& book, const std::vector<Operation>& operations) {
    std::uint64_t checksum{0};
    for (const Operation& operation : operations) {
        const auto result{apply(book, operation)};
        checksum += result.executed_quantity;
        checksum += result.remaining_quantity;
        checksum += result.execution_count;
        checksum += static_cast<std::uint8_t>(result.status);
    }
    const auto stats{book.stats()};
    return checksum + stats.execution_count + stats.active_orders;
}

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted_values, double fraction) {
    const auto index{static_cast<std::size_t>(
        fraction * static_cast<double>(sorted_values.size() - 1))};
    return sorted_values[index];
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
        } else if (argument == "--help") {
            std::cout << "usage: orderbook_benchmark [--operations N] [--iterations N]\n";
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

        const auto warmup_workload{
            make_workload(std::min<std::size_t>(workload.size(), 14'000))};
        orderbook::OrderBook warmup_book{orderbook::BookConfig{16, &events}};
        const std::uint64_t warmup_checksum{run_workload(warmup_book, warmup_workload)};
        require_valid(warmup_book, "warmup");

        orderbook::OrderBook throughput_book{orderbook::BookConfig{16, &events}};
        const auto throughput_start{Clock::now()};
        std::uint64_t throughput_checksum{0};
        for (std::size_t iteration{0}; iteration < config.throughput_iterations; ++iteration) {
            throughput_checksum += run_workload(throughput_book, workload);
        }
        const auto throughput_end{Clock::now()};
        require_valid(throughput_book, "throughput");
        const double elapsed_seconds{
            std::chrono::duration<double>(throughput_end - throughput_start).count()};

        orderbook::OrderBook latency_book{orderbook::BookConfig{16, &events}};
        std::vector<std::uint64_t> latencies;
        latencies.reserve(workload.size());
        std::uint64_t latency_checksum{0};
        for (const Operation& operation : workload) {
            const auto start{Clock::now()};
            const auto result{apply(latency_book, operation)};
            const auto end{Clock::now()};
            latencies.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
            latency_checksum += result.executed_quantity;
            latency_checksum += result.remaining_quantity;
            latency_checksum += result.execution_count;
        }
        require_valid(latency_book, "latency");
        std::sort(latencies.begin(), latencies.end());

#ifdef NDEBUG
        constexpr std::string_view build_type{"release"};
#else
        constexpr std::string_view build_type{"debug (not representative)"};
#endif

        const std::size_t measured_throughput_operations{
            workload.size() * config.throughput_iterations};

        std::cout << "Order book benchmark baseline\n"
                  << "  build:       " << build_type << '\n'
                  << "  workload:    deterministic GTC adds, market/IOC matching, cancels\n"
                  << "  operations:  " << measured_throughput_operations
                  << " (" << workload.size() << " x "
                  << config.throughput_iterations << ")\n"
                  << "  elapsed:     " << std::fixed << std::setprecision(6)
                  << elapsed_seconds << " s\n"
                  << "  throughput:  " << std::setprecision(0)
                  << static_cast<double>(measured_throughput_operations) / elapsed_seconds
                  << " ops/s\n"
                  << "  latency p50: " << percentile(latencies, 0.50) << " ns\n"
                  << "  latency p95: " << percentile(latencies, 0.95) << " ns\n"
                  << "  latency p99: " << percentile(latencies, 0.99) << " ns\n"
                  << "  latency max: " << latencies.back() << " ns\n"
                  << "  checksum:    "
                  << warmup_checksum + throughput_checksum + latency_checksum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
