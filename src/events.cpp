#include "orderbook/events.hpp"

#include <iomanip>

namespace orderbook {

RecordingEventSink::RecordingEventSink(
    std::size_t execution_capacity, std::size_t rejection_capacity)
    : execution_capacity_{execution_capacity}, rejection_capacity_{rejection_capacity} {
    executions_.reserve(execution_capacity_);
    rejections_.reserve(rejection_capacity_);
}

void RecordingEventSink::on_execution(const ExecutionEvent& event) noexcept {
    if (executions_.size() < execution_capacity_) {
        executions_.push_back(event);
    } else {
        ++dropped_executions_;
    }
}

void RecordingEventSink::on_rejection(const RejectionEvent& event) noexcept {
    if (rejections_.size() < rejection_capacity_) {
        rejections_.push_back(event);
    } else {
        ++dropped_rejections_;
    }
}

void RecordingEventSink::clear() noexcept {
    executions_.clear();
    rejections_.clear();
    dropped_executions_ = 0;
    dropped_rejections_ = 0;
}

void PrintingEventSink::on_execution(const ExecutionEvent& event) noexcept {
    try {
        output_ << "EXECUTION sequence=" << event.sequence
                << " quantity=" << event.quantity
                << " price=" << std::fixed << std::setprecision(4)
                << price_to_double(event.price)
                << " maker=" << event.maker_id.value()
                << " taker=" << event.taker_id.value()
                << " taker_side=" << to_string(event.taker_side) << '\n';
    } catch (...) {
        failed_ = true;
    }
}

void PrintingEventSink::on_rejection(const RejectionEvent& event) noexcept {
    try {
        output_ << "REJECTION order=" << event.order_id.value()
                << " status=" << to_string(event.status) << '\n';
    } catch (...) {
        failed_ = true;
    }
}

}  // namespace orderbook
