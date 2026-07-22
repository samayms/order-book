#include "orderbook/engine.hpp"

#include <exception>
#include <type_traits>
#include <utility>

namespace orderbook {

OrderBookEngine::OrderBookEngine(BookConfig config)
    : book_{config}, worker_{&OrderBookEngine::run, this} {}

OrderBookEngine::~OrderBookEngine() {
    shutdown();
}

std::future<SubmitResult> OrderBookEngine::enqueue(OrderRequest request) {
    std::promise<SubmitResult> completion;
    std::future<SubmitResult> result{completion.get_future()};
    {
        const std::lock_guard lock{queue_mutex_};
        if (!accepting_) {
            const Quantity remaining{std::visit(
                [](const auto& value) -> Quantity {
                    using Request = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Request, CancelOrder>) {
                        return 0;
                    } else {
                        return value.quantity;
                    }
                },
                request)};
            completion.set_value(
                SubmitResult{Status::engine_stopped, 0, remaining, 0});
            return result;
        }
        commands_.push(Command{std::move(request), std::move(completion)});
    }
    command_ready_.notify_one();
    return result;
}

void OrderBookEngine::shutdown() {
    std::call_once(shutdown_once_, [this] {
        {
            const std::lock_guard lock{queue_mutex_};
            accepting_ = false;
        }
        command_ready_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    });
}

bool OrderBookEngine::accepting() const {
    const std::lock_guard lock{queue_mutex_};
    return accepting_;
}

void OrderBookEngine::run() {
    while (true) {
        Command command;
        {
            std::unique_lock lock{queue_mutex_};
            command_ready_.wait(lock, [this] {
                return !commands_.empty() || !accepting_;
            });
            if (commands_.empty()) {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop();
        }

        try {
            command.completion.set_value(dispatch(command.request));
        } catch (...) {
            command.completion.set_exception(std::current_exception());
        }
    }
}

SubmitResult OrderBookEngine::dispatch(const OrderRequest& request) {
    return std::visit(
        [this](const auto& value) -> SubmitResult {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, CancelOrder>) {
                return SubmitResult{book_.cancel(value.id), 0, 0, 0};
            } else {
                return book_.submit(value);
            }
        },
        request);
}

}  // namespace orderbook
