#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

#include "kerndb/storage/buffer_pool.h"
#include "test_framework.h"
#include "temp_directory.h"

namespace {

using kerndb::PageId;
using kerndb::storage::BufferPoolManager;
using kerndb::storage::DiskManager;
using kerndb::storage::Page;
using kerndb::storage::PageType;
using kerndb::storage::PinnedPage;

[[nodiscard]] std::unique_ptr<BufferPoolManager> OpenPool(
    const std::filesystem::path& path,
    std::size_t frames,
    kerndb::MetricsRegistry* metrics = nullptr) {
    auto opened = BufferPoolManager::Open(path, true, frames, metrics);
    KERNDB_EXPECT(opened.ok());
    return std::move(opened).value();
}

void Release(PinnedPage& page) {
    KERNDB_EXPECT(page.Release().ok());
}

void CreatePages(const std::filesystem::path& path, std::uint64_t count) {
    const auto disk = DiskManager::Open(path, true);
    KERNDB_EXPECT(disk.ok());
    for (std::uint64_t page_number = 0U; page_number < count; ++page_number) {
        const auto page = Page::Create(PageType::kHeap, PageId{page_number});
        KERNDB_EXPECT(page.ok());
        KERNDB_EXPECT(disk.value()->WritePage(page.value()).ok());
    }
}

}  // namespace

KERNDB_TEST(BufferPoolCreatesFetchesAndMaintainsGuardPins) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    kerndb::MetricsRegistry metrics;

    {
        auto pool = OpenPool(path, 2U, &metrics);
        KERNDB_EXPECT_EQ(std::size_t{2U}, pool->frame_count());

        auto created = pool->NewPage(PageType::kHeap);
        KERNDB_EXPECT(created.ok());
        PinnedPage created_guard = std::move(created).value();
        KERNDB_EXPECT_EQ(PageId{0U}, created_guard.page_id());
        KERNDB_EXPECT(created_guard.pinned());

        auto fetched = pool->FetchPage(PageId{0U});
        KERNDB_EXPECT(fetched.ok());
        PinnedPage fetched_guard = std::move(fetched).value();
        KERNDB_EXPECT_EQ(PageId{0U}, fetched_guard.page_id());
        Release(fetched_guard);
        Release(created_guard);
        KERNDB_EXPECT(pool->FlushAllPages().ok());
    }

    auto reopened = OpenPool(path, 2U, &metrics);
    auto fetched_after_restart = reopened->FetchPage(PageId{0U});
    KERNDB_EXPECT(fetched_after_restart.ok());
    PinnedPage fetched_guard = std::move(fetched_after_restart).value();
    Release(fetched_guard);

    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.page_hits"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.page_misses"));
}

KERNDB_TEST(BufferPoolFlushesDirtyPages) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    kerndb::MetricsRegistry metrics;
    auto pool = OpenPool(path, 1U, &metrics);

    auto created = pool->NewPage(PageType::kHeap);
    KERNDB_EXPECT(created.ok());
    PinnedPage guard = std::move(created).value();
    guard.mutable_page().mutable_payload()[0U] = std::byte{0x5AU};
    Release(guard);

    const auto before_flush = DiskManager::Open(path, false);
    KERNDB_EXPECT(before_flush.ok());
    const auto before_page = before_flush.value()->ReadPage(PageId{0U});
    KERNDB_EXPECT(before_page.ok());
    KERNDB_EXPECT_EQ(std::byte{0U}, before_page.value().payload()[0U]);

    KERNDB_EXPECT(pool->FlushPage(PageId{0U}).ok());
    const auto after_flush = before_flush.value()->ReadPage(PageId{0U});
    KERNDB_EXPECT(after_flush.ok());
    KERNDB_EXPECT_EQ(std::byte{0x5AU}, after_flush.value().payload()[0U]);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.dirty_flushes"));
}

KERNDB_TEST(BufferPoolEvictsTheLeastRecentlyUnpinnedFrame) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    CreatePages(path, 3U);
    kerndb::MetricsRegistry metrics;
    auto pool = OpenPool(path, 2U, &metrics);

    auto page_zero = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(page_zero.ok());
    PinnedPage zero_guard = std::move(page_zero).value();
    Release(zero_guard);

    auto page_one = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(page_one.ok());
    PinnedPage one_guard = std::move(page_one).value();
    Release(one_guard);

    auto page_zero_again = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(page_zero_again.ok());
    PinnedPage zero_again_guard = std::move(page_zero_again).value();
    Release(zero_again_guard);

    auto page_two = pool->FetchPage(PageId{2U});
    KERNDB_EXPECT(page_two.ok());
    PinnedPage two_guard = std::move(page_two).value();
    Release(two_guard);

    auto page_one_again = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(page_one_again.ok());
    PinnedPage one_again_guard = std::move(page_one_again).value();
    Release(one_again_guard);

    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.page_hits"));
    KERNDB_EXPECT_EQ(std::uint64_t{4U}, metrics.CounterValue("buffer_pool.page_misses"));
    KERNDB_EXPECT_EQ(std::uint64_t{2U}, metrics.CounterValue("buffer_pool.evictions"));
}

KERNDB_TEST(BufferPoolProtectsPinnedPagesAndReportsFullPoolFailure) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    CreatePages(path, 2U);
    kerndb::MetricsRegistry metrics;
    auto pool = OpenPool(path, 1U, &metrics);

    auto page_zero = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(page_zero.ok());
    PinnedPage zero_guard = std::move(page_zero).value();

    const auto blocked_fetch = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(!blocked_fetch.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kResourceExhausted, blocked_fetch.status().code());
    const auto blocked_allocation = pool->NewPage(PageType::kHeap);
    KERNDB_EXPECT(!blocked_allocation.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kResourceExhausted, blocked_allocation.status().code());

    Release(zero_guard);
    auto page_one = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(page_one.ok());
    PinnedPage one_guard = std::move(page_one).value();
    Release(one_guard);

    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.failed_fetches"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.failed_allocations"));
}

KERNDB_TEST(BufferPoolDeletesOnlyUnpinnedPages) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    auto pool = OpenPool(path, 1U);

    auto created = pool->NewPage(PageType::kHeap);
    KERNDB_EXPECT(created.ok());
    PinnedPage guard = std::move(created).value();
    const auto pinned_delete = pool->DeletePage(PageId{0U});
    KERNDB_EXPECT(!pinned_delete.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kResourceExhausted, pinned_delete.code());

    Release(guard);
    KERNDB_EXPECT(pool->DeletePage(PageId{0U}).ok());
    const auto page_count = pool->PageCount();
    KERNDB_EXPECT(page_count.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{0U}, page_count.value());

    const auto deleted_fetch = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(!deleted_fetch.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kOutOfRange, deleted_fetch.status().code());
}

KERNDB_TEST(BufferPoolPersistsDirtyPagesAcrossAnOrderlyRestart) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";

    {
        auto pool = OpenPool(path, 1U);
        auto created = pool->NewPage(PageType::kHeap);
        KERNDB_EXPECT(created.ok());
        PinnedPage guard = std::move(created).value();
        guard.mutable_page().mutable_payload()[3U] = std::byte{0x77U};
        Release(guard);
    }

    auto reopened = OpenPool(path, 1U);
    auto fetched = reopened->FetchPage(PageId{0U});
    KERNDB_EXPECT(fetched.ok());
    PinnedPage guard = std::move(fetched).value();
    KERNDB_EXPECT_EQ(std::byte{0x77U}, guard.page().payload()[3U]);
    Release(guard);
}

KERNDB_TEST(BufferPoolRejectsCorruptionOnACacheMissWithoutEvictingAValidPage) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    CreatePages(path, 2U);
    kerndb::MetricsRegistry metrics;
    auto pool = OpenPool(path, 1U, &metrics);

    auto valid_page = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(valid_page.ok());
    PinnedPage valid_guard = std::move(valid_page).value();
    Release(valid_guard);

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        KERNDB_EXPECT(file.is_open());
        file.seekp(static_cast<std::streamoff>(kerndb::kInitialPageSizeBytes + 100U));
        file.put(static_cast<char>(0xA5));
        file.flush();
        KERNDB_EXPECT(file.good());
    }

    const auto corrupt_page = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(!corrupt_page.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, corrupt_page.status().code());

    auto still_resident = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(still_resident.ok());
    PinnedPage resident_guard = std::move(still_resident).value();
    Release(resident_guard);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.page_hits"));
    KERNDB_EXPECT_EQ(std::uint64_t{2U}, metrics.CounterValue("buffer_pool.page_misses"));
    KERNDB_EXPECT_EQ(std::uint64_t{0U}, metrics.CounterValue("buffer_pool.evictions"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.failed_fetches"));
}

KERNDB_TEST(BufferPoolEmitsCacheFlushEvictionAndFailureTelemetry) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    CreatePages(path, 2U);
    kerndb::MetricsRegistry metrics;
    auto pool = OpenPool(path, 1U, &metrics);

    auto page_zero = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(page_zero.ok());
    PinnedPage zero_guard = std::move(page_zero).value();
    Release(zero_guard);

    auto page_zero_hit = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(page_zero_hit.ok());
    PinnedPage zero_hit_guard = std::move(page_zero_hit).value();
    zero_hit_guard.mutable_page().mutable_payload()[0U] = std::byte{0x11U};
    Release(zero_hit_guard);

    auto page_one = pool->FetchPage(PageId{1U});
    KERNDB_EXPECT(page_one.ok());
    PinnedPage one_guard = std::move(page_one).value();

    const auto blocked_fetch = pool->FetchPage(PageId{0U});
    KERNDB_EXPECT(!blocked_fetch.ok());
    const auto blocked_allocation = pool->NewPage(PageType::kHeap);
    KERNDB_EXPECT(!blocked_allocation.ok());
    Release(one_guard);

    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.page_hits"));
    KERNDB_EXPECT_EQ(std::uint64_t{3U}, metrics.CounterValue("buffer_pool.page_misses"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.evictions"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.dirty_flushes"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.failed_fetches"));
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, metrics.CounterValue("buffer_pool.failed_allocations"));
}
