#include "kerndb/telemetry.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace kerndb {

Status MetricsRegistry::IncrementCounter(std::string_view name, std::uint64_t amount) {
    if (name.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "metric name cannot be empty");
    }

    std::scoped_lock lock(mutex_);
    std::uint64_t& counter = counters_[std::string(name)];
    if (amount > std::numeric_limits<std::uint64_t>::max() - counter) {
        return Status::Error(ErrorCode::kOverflow, "metric counter would overflow")
            .WithContext("metric", std::string(name));
    }
    counter += amount;
    return Status::Ok();
}

std::uint64_t MetricsRegistry::CounterValue(std::string_view name) const {
    std::scoped_lock lock(mutex_);
    const auto iterator = counters_.find(std::string(name));
    return iterator == counters_.end() ? 0U : iterator->second;
}

MetricSnapshot MetricsRegistry::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return MetricSnapshot{
        .counters = counters_,
    };
}

InMemoryEventSink::InMemoryEventSink(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0U) {
        throw std::invalid_argument("event sink capacity must be greater than zero");
    }
}

Status InMemoryEventSink::Publish(DatabaseEvent event) {
    if (event.component.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "event component cannot be empty");
    }
    if (event.name.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "event name cannot be empty");
    }

    std::scoped_lock lock(mutex_);
    if (next_sequence_ == 0U) {
        return Status::Error(ErrorCode::kOverflow, "event sequence space is exhausted");
    }

    event.sequence = next_sequence_;
    ++next_sequence_;
    event.timestamp = std::chrono::system_clock::now();
    if (events_.size() == capacity_) {
        events_.pop_front();
    }
    events_.push_back(std::move(event));
    return Status::Ok();
}

EventSnapshot InMemoryEventSink::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return EventSnapshot{
        .events = {events_.begin(), events_.end()},
        .next_sequence = next_sequence_,
    };
}

}  // namespace kerndb
