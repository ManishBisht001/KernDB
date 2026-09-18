#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "kerndb/telemetry.h"
#include "test_framework.h"

namespace {

kerndb::DatabaseEvent MakeBufferEvent(std::string name) {
    kerndb::DatabaseEvent event;
    event.component = "buffer";
    event.name = std::move(name);
    return event;
}

}  // namespace

KERNDB_TEST(MetricsRegistryCapturesCountersAndIsThreadSafe) {
    kerndb::MetricsRegistry metrics;
    constexpr std::size_t kThreadCount = 4U;
    constexpr std::size_t kIncrementsPerThread = 100U;
    std::array<std::thread, kThreadCount> threads;

    for (std::thread& thread : threads) {
        thread = std::thread([&metrics] {
            for (std::size_t index = 0U; index < kIncrementsPerThread; ++index) {
                const kerndb::Status status = metrics.IncrementCounter("buffer_pool.hits");
                if (!status.ok()) {
                    throw std::runtime_error(status.ToString());
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const auto snapshot = metrics.Snapshot();
    KERNDB_EXPECT_EQ(
        static_cast<std::uint64_t>(kThreadCount * kIncrementsPerThread),
        metrics.CounterValue("buffer_pool.hits"));
    KERNDB_EXPECT_EQ(std::size_t{1U}, snapshot.counters.size());
}

KERNDB_TEST(MetricsRegistryRejectsInvalidNamesAndOverflow) {
    kerndb::MetricsRegistry metrics;

    KERNDB_EXPECT(!metrics.IncrementCounter("").ok());
    KERNDB_EXPECT(metrics.IncrementCounter(
        "events",
        std::numeric_limits<std::uint64_t>::max()).ok());
    const kerndb::Status overflow = metrics.IncrementCounter("events");
    KERNDB_EXPECT(!overflow.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kOverflow, overflow.code());
}

KERNDB_TEST(InMemoryEventSinkKeepsTheNewestEventsInOrder) {
    kerndb::InMemoryEventSink sink{2U};
    KERNDB_EXPECT(sink.Publish(MakeBufferEvent("hit")).ok());
    KERNDB_EXPECT(sink.Publish(MakeBufferEvent("miss")).ok());
    KERNDB_EXPECT(sink.Publish(MakeBufferEvent("evict")).ok());

    const kerndb::EventSnapshot snapshot = sink.Snapshot();
    KERNDB_EXPECT_EQ(std::size_t{2U}, snapshot.events.size());
    KERNDB_EXPECT_EQ(std::string("miss"), snapshot.events[0].name);
    KERNDB_EXPECT_EQ(std::uint64_t{2U}, snapshot.events[0].sequence);
    KERNDB_EXPECT_EQ(std::string("evict"), snapshot.events[1].name);
    KERNDB_EXPECT_EQ(std::uint64_t{4U}, snapshot.next_sequence);
}
