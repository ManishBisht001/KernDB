#include <cstdint>
#include <filesystem>
#include <string>

#include "kerndb/storage/heap.h"
#include "kerndb/storage/page_manager.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(SlottedPageKeepsSlotsStableAndValidatesReferences) {
    const auto page = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{0U});
    KERNDB_EXPECT(page.ok());
    const auto initialized = kerndb::storage::SlottedPage::Initialize(page.value());
    KERNDB_EXPECT(initialized.ok());
    auto slotted_page = initialized.value();

    const std::vector<std::byte> first{std::byte{0x01U}, std::byte{0x02U}};
    const std::vector<std::byte> second{std::byte{0x03U}};
    const auto first_slot = slotted_page.Insert(first);
    const auto second_slot = slotted_page.Insert(second);
    KERNDB_EXPECT(first_slot.ok() && first_slot.value().has_value());
    KERNDB_EXPECT(second_slot.ok() && second_slot.value().has_value());
    KERNDB_EXPECT_EQ(std::uint32_t{0U}, first_slot.value()->value());
    KERNDB_EXPECT_EQ(std::uint32_t{1U}, second_slot.value()->value());
    const auto first_read = slotted_page.Read(first_slot.value().value());
    KERNDB_EXPECT(first_read.ok());
    KERNDB_EXPECT_EQ(first, first_read.value());
    KERNDB_EXPECT(!slotted_page.Read(kerndb::SlotId{9U}).ok());
}

KERNDB_TEST(SlottedPageUsesTheLastByteOfAnEmptyPageWithoutOverlappingSlots) {
    const auto page = kerndb::storage::Page::Create(
        kerndb::storage::PageType::kHeap,
        kerndb::PageId{0U});
    KERNDB_EXPECT(page.ok());
    const auto initialized = kerndb::storage::SlottedPage::Initialize(page.value());
    KERNDB_EXPECT(initialized.ok());
    auto slotted_page = initialized.value();

    const std::vector<std::byte> largest_record(
        kerndb::kInitialPageSizeBytes - kerndb::storage::kPageHeaderSize - 8U - 12U,
        std::byte{0xABU});
    const auto inserted = slotted_page.Insert(largest_record);
    KERNDB_EXPECT(inserted.ok() && inserted.value().has_value());
    KERNDB_EXPECT_EQ(std::size_t{0U}, slotted_page.FreeBytes());
    const auto no_space = slotted_page.Insert(std::vector<std::byte>{std::byte{0xCDU}});
    KERNDB_EXPECT(no_space.ok());
    KERNDB_EXPECT(!no_space.value().has_value());
}

KERNDB_TEST(TableHeapPersistsAndScansTuplesThroughTheBufferPool) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "table.dat";
    const kerndb::Schema schema{
        {.name = "id", .type = kerndb::ColumnType::kInt},
        {.name = "name", .type = kerndb::ColumnType::kText},
    };
    kerndb::RecordId rid;
    {
        auto manager = kerndb::storage::PageManager::Open(path, true);
        KERNDB_EXPECT(manager.ok());
        kerndb::storage::TableHeap heap{manager.value()};
        const auto inserted = heap.Insert(schema, kerndb::Tuple{std::int64_t{1}, std::string("Ada")});
        KERNDB_EXPECT(inserted.ok());
        rid = inserted.value();
    }

    auto reopened_manager = kerndb::storage::PageManager::Open(path, false);
    KERNDB_EXPECT(reopened_manager.ok());
    kerndb::storage::TableHeap reopened_heap{reopened_manager.value()};
    const auto read = reopened_heap.Read(schema, rid);
    KERNDB_EXPECT(read.ok());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(read.value()[1]));
    const auto scan = reopened_heap.Scan(schema);
    KERNDB_EXPECT(scan.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, scan.value().size());
}

KERNDB_TEST(PageManagerAllocatesContiguousPersistentPageIds) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "pages.dat";
    auto manager = kerndb::storage::PageManager::Open(path, true);
    KERNDB_EXPECT(manager.ok());

    const auto first = manager.value().AllocatePage(kerndb::storage::PageType::kHeap);
    const auto second = manager.value().AllocatePage(kerndb::storage::PageType::kHeap);
    KERNDB_EXPECT(first.ok());
    KERNDB_EXPECT(second.ok());
    KERNDB_EXPECT_EQ(kerndb::PageId{0U}, first.value().page_id());
    KERNDB_EXPECT_EQ(kerndb::PageId{1U}, second.value().page_id());

    const auto count = manager.value().PageCount();
    KERNDB_EXPECT(count.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{2U}, count.value());

    auto reopened = kerndb::storage::PageManager::Open(path, false);
    KERNDB_EXPECT(reopened.ok());
    const auto second_page = reopened.value().ReadPage(kerndb::PageId{1U});
    KERNDB_EXPECT(second_page.ok());
    KERNDB_EXPECT_EQ(kerndb::PageId{1U}, second_page.value().page_id());
}
