#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "kerndb/storage/bplus_tree.h"
#include "kerndb/storage/buffer_pool.h"
#include "kerndb/storage/page_manager.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(BufferPoolSerializesConcurrentPageGuardAccess) {
    kerndb::test::TemporaryDirectory directory;
    auto opened = kerndb::storage::BufferPoolManager::Open(
        directory.path() / "shared_page.dat",
        true,
        2U);
    KERNDB_EXPECT(opened.ok());
    auto pool = std::move(opened).value();
    auto created = pool->NewPage(kerndb::storage::PageType::kHeap);
    KERNDB_EXPECT(created.ok());
    kerndb::storage::PinnedPage initial_guard = std::move(created).value();
    KERNDB_EXPECT(initial_guard.Release().ok());

    constexpr std::size_t kThreadCount = 6U;
    constexpr std::size_t kIncrementsPerThread = 120U;
    std::atomic_size_t failures{0U};
    std::array<std::thread, kThreadCount> threads;
    for (std::thread& thread : threads) {
        thread = std::thread([&] {
            for (std::size_t iteration = 0U; iteration < kIncrementsPerThread; ++iteration) {
                auto fetched = pool->FetchPage(kerndb::PageId{0U});
                if (!fetched.ok()) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                    continue;
                }
                kerndb::storage::PinnedPage guard = std::move(fetched).value();
                auto payload = guard.mutable_page().mutable_payload();
                std::uint64_t value = 0U;
                std::memcpy(&value, payload.data(), sizeof(value));
                ++value;
                std::memcpy(payload.data(), &value, sizeof(value));
                if (!guard.Release().ok()) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    KERNDB_EXPECT_EQ(std::size_t{0U}, failures.load(std::memory_order_relaxed));
    KERNDB_EXPECT(pool->FlushAllPages().ok());

    auto fetched = pool->FetchPage(kerndb::PageId{0U});
    KERNDB_EXPECT(fetched.ok());
    kerndb::storage::PinnedPage guard = std::move(fetched).value();
    std::uint64_t final_value = 0U;
    std::memcpy(&final_value, guard.page().payload().data(), sizeof(final_value));
    KERNDB_EXPECT(guard.Release().ok());
    KERNDB_EXPECT_EQ(
        static_cast<std::uint64_t>(kThreadCount * kIncrementsPerThread),
        final_value);
}

KERNDB_TEST(BPlusTreeSupportsConcurrentInsertionsAndLookups) {
    kerndb::test::TemporaryDirectory directory;
    auto manager = kerndb::storage::PageManager::Open(directory.path() / "concurrent_tree.dat", true, 8U);
    KERNDB_EXPECT(manager.ok());
    auto opened_tree = kerndb::storage::BPlusTree::Create(manager.value());
    KERNDB_EXPECT(opened_tree.ok());
    kerndb::storage::BPlusTree& tree = opened_tree.value();

    constexpr std::size_t kThreadCount = 4U;
    constexpr std::int64_t kKeysPerThread = 40;
    std::atomic_size_t failures{0U};
    std::array<std::thread, kThreadCount> threads;
    for (std::size_t thread_id = 0U; thread_id < kThreadCount; ++thread_id) {
        threads[thread_id] = std::thread([thread_id, &tree, &failures] {
            for (std::int64_t offset = 0; offset < kKeysPerThread; ++offset) {
                const std::int64_t key =
                    static_cast<std::int64_t>(thread_id) * kKeysPerThread + offset + 1;
                const kerndb::RecordId record_id{
                    .page_id = kerndb::PageId{static_cast<std::uint64_t>(key)},
                    .slot_id = kerndb::SlotId{0U},
                };
                if (!tree.Insert(key, record_id).ok()) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    KERNDB_EXPECT_EQ(std::size_t{0U}, failures.load(std::memory_order_relaxed));

    constexpr std::int64_t kTotalKeys = static_cast<std::int64_t>(kThreadCount) * kKeysPerThread;
    const auto entries = tree.ScanAll();
    KERNDB_EXPECT(entries.ok());
    KERNDB_EXPECT_EQ(static_cast<std::size_t>(kTotalKeys), entries.value().size());
    for (std::int64_t key = 1; key <= kTotalKeys; ++key) {
        const auto found = tree.Search(key);
        KERNDB_EXPECT(found.ok());
        KERNDB_EXPECT_EQ(kerndb::PageId{static_cast<std::uint64_t>(key)}, found.value().page_id);
    }
}
