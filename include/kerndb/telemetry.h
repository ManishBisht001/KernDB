#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"

namespace kerndb {

enum class EventSeverity : std::uint8_t {
    kDebug,
    kInfo,
    kWarning,
    kError,
};

struct MetricSnapshot {
    std::map<std::string, std::uint64_t> counters;
};

class MetricsRegistry {
public:
    [[nodiscard]] Status IncrementCounter(std::string_view name, std::uint64_t amount = 1U);
    [[nodiscard]] std::uint64_t CounterValue(std::string_view name) const;
    [[nodiscard]] MetricSnapshot Snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
};

struct DatabaseEvent {
    EventSeverity severity{EventSeverity::kInfo};
    std::string component;
    std::string name;
    std::map<std::string, std::string> fields;
    std::optional<QueryId> query_id;
    std::optional<TransactionId> transaction_id;
    std::uint64_t sequence{0U};
    std::chrono::system_clock::time_point timestamp{};
};

struct EventSnapshot {
    std::vector<DatabaseEvent> events;
    std::uint64_t next_sequence{1U};
};

class EventSink {
public:
    virtual ~EventSink() = default;

    virtual Status Publish(DatabaseEvent event) = 0;
};

class InMemoryEventSink final : public EventSink {
public:
    explicit InMemoryEventSink(std::size_t capacity);

    [[nodiscard]] Status Publish(DatabaseEvent event) override;
    [[nodiscard]] EventSnapshot Snapshot() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<DatabaseEvent> events_;
    std::uint64_t next_sequence_{1U};
};

}  // namespace kerndb
