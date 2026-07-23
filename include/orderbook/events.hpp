#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <ostream>
#include <vector>

namespace orderbook {

struct ExecutionEvent {
    SequenceNumber sequence{0};
    OrderId maker_id;
    OrderId taker_id;
    Side taker_side{Side::buy};
    Price price;
    Quantity quantity{0};
};

struct RejectionEvent {
    OrderId order_id;
    Status status{Status::accepted};
};

class EventSink {
public:
    virtual ~EventSink() = default;
    EventSink() = default;
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;
    EventSink(EventSink&&) = delete;
    EventSink& operator=(EventSink&&) = delete;

    virtual void on_execution(const ExecutionEvent& event) noexcept = 0;
    virtual void on_rejection(const RejectionEvent& event) noexcept = 0;
};

class NullEventSink final : public EventSink {
public:
    void on_execution(const ExecutionEvent&) noexcept override {}
    void on_rejection(const RejectionEvent&) noexcept override {}
};

class RecordingEventSink final : public EventSink {
public:
    explicit RecordingEventSink(std::size_t execution_capacity = 1'024,
                                std::size_t rejection_capacity = 128);

    void on_execution(const ExecutionEvent& event) noexcept override;
    void on_rejection(const RejectionEvent& event) noexcept override;

    [[nodiscard]] const std::vector<ExecutionEvent>& executions() const noexcept {
        return executions_;
    }
    [[nodiscard]] const std::vector<RejectionEvent>& rejections() const noexcept {
        return rejections_;
    }
    [[nodiscard]] std::size_t dropped_execution_count() const noexcept {
        return dropped_executions_;
    }
    [[nodiscard]] std::size_t dropped_rejection_count() const noexcept {
        return dropped_rejections_;
    }
    void clear() noexcept;

private:
    std::vector<ExecutionEvent> executions_;
    std::vector<RejectionEvent> rejections_;
    std::size_t execution_capacity_;
    std::size_t rejection_capacity_;
    std::size_t dropped_executions_{0};
    std::size_t dropped_rejections_{0};
};

class PrintingEventSink final : public EventSink {
public:
    explicit PrintingEventSink(std::ostream& output) noexcept : output_{output} {}

    void on_execution(const ExecutionEvent& event) noexcept override;
    void on_rejection(const RejectionEvent& event) noexcept override;
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::ostream& output_;
    bool failed_{false};
};

}  // namespace orderbook
