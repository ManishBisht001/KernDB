#include <cstddef>
#include <filesystem>
#include <fstream>

#include "kerndb/storage/disk_manager.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(DiskManagerWritesReadsAndCountsFixedSizePages) {
    kerndb::test::TemporaryDirectory directory;
    const auto manager = kerndb::storage::DiskManager::Open(directory.path() / "pages.dat", true);
    KERNDB_EXPECT(manager.ok());
    const auto initial_page_count = manager.value()->PageCount();
    KERNDB_EXPECT(initial_page_count.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{0U}, initial_page_count.value());

    const auto page = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{0U});
    KERNDB_EXPECT(page.ok());
    KERNDB_EXPECT(manager.value()->WritePage(page.value()).ok());
    const auto final_page_count = manager.value()->PageCount();
    KERNDB_EXPECT(final_page_count.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, final_page_count.value());

    const auto read = manager.value()->ReadPage(kerndb::PageId{0U});
    KERNDB_EXPECT(read.ok());
    KERNDB_EXPECT_EQ(kerndb::PageId{0U}, read.value().page_id());
    KERNDB_EXPECT(!manager.value()->ReadPage(kerndb::PageId{1U}).ok());
}

KERNDB_TEST(DiskManagerRejectsTruncatedFiles) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "truncated.dat";
    {
        std::ofstream file(path, std::ios::binary);
        file.put('\0');
    }
    const auto manager = kerndb::storage::DiskManager::Open(path, false);
    KERNDB_EXPECT(!manager.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, manager.status().code());
}

KERNDB_TEST(DiskManagerRejectsChecksumCorruptionPersistedToDisk) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    const auto manager = kerndb::storage::DiskManager::Open(path, true);
    KERNDB_EXPECT(manager.ok());

    const auto page = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{0U});
    KERNDB_EXPECT(page.ok());
    KERNDB_EXPECT(manager.value()->WritePage(page.value()).ok());

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    KERNDB_EXPECT(file.is_open());
    file.seekp(100);
    file.put(static_cast<char>(0xA5));
    file.flush();
    KERNDB_EXPECT(file.good());

    const auto reread = manager.value()->ReadPage(kerndb::PageId{0U});
    KERNDB_EXPECT(!reread.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, reread.status().code());
}
