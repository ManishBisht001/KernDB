#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "kerndb/storage/heap.h"
#include "kerndb/storage/page_manager.h"
#include "storage/catalog_store.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(CatalogStorePersistsStableTableMetadata) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "catalog.dat";
    const kerndb::InMemoryTable expected{
        .id = kerndb::TableId{7U},
        .name = "people",
        .schema = {
            {.name = "id", .type = kerndb::ColumnType::kInt},
            {.name = "name", .type = kerndb::ColumnType::kText},
        },
        .tuples = {},
    };

    auto catalog = kerndb::storage::CatalogStore::Open(path);
    KERNDB_EXPECT(catalog.ok());
    KERNDB_EXPECT(catalog.value().Append(expected).ok());

    const auto reopened = kerndb::storage::CatalogStore::Open(path);
    KERNDB_EXPECT(reopened.ok());
    const auto tables = reopened.value().Load();
    KERNDB_EXPECT(tables.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, tables.value().size());
    KERNDB_EXPECT_EQ(expected.id, tables.value()[0].id);
    KERNDB_EXPECT_EQ(expected.name, tables.value()[0].name);
    KERNDB_EXPECT_EQ(expected.schema, tables.value()[0].schema);
}

KERNDB_TEST(CatalogStoreRejectsMalformedCatalogRecords) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "catalog.dat";
    auto manager = kerndb::storage::PageManager::Open(path, true);
    KERNDB_EXPECT(manager.ok());
    const auto page = manager.value().AllocatePage(kerndb::storage::PageType::kCatalog);
    KERNDB_EXPECT(page.ok());
    const auto initialized = kerndb::storage::SlottedPage::Initialize(page.value());
    KERNDB_EXPECT(initialized.ok());
    auto slotted_page = initialized.value();
    const std::vector<std::byte> malformed{std::byte{0x01U}};
    const auto slot = slotted_page.Insert(malformed);
    KERNDB_EXPECT(slot.ok() && slot.value().has_value());
    KERNDB_EXPECT(manager.value().WritePage(slotted_page.page()).ok());

    const auto catalog = kerndb::storage::CatalogStore::Open(path);
    KERNDB_EXPECT(catalog.ok());
    const auto tables = catalog.value().Load();
    KERNDB_EXPECT(!tables.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, tables.status().code());
}
