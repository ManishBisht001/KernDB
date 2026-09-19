#include <cstdint>
#include <filesystem>
#include <string>

#include "catalog/catalog.h"
#include "storage/index_catalog_store.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(IndexCatalogStorePersistsTheNewestVersionOfIndexMetadata) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "indexes.dat";
    auto store = kerndb::storage::IndexCatalogStore::Open(path);
    KERNDB_EXPECT(store.ok());

    const kerndb::InMemoryIndex original{
        .id = kerndb::IndexId{3U},
        .name = "people_id_idx",
        .table_id = kerndb::TableId{1U},
        .column_index = 0U,
        .root_page_id = kerndb::PageId{0U},
    };
    KERNDB_EXPECT(store.value().Append(original).ok());
    auto updated = original;
    updated.root_page_id = kerndb::PageId{7U};
    KERNDB_EXPECT(store.value().Append(updated).ok());

    const auto reopened = kerndb::storage::IndexCatalogStore::Open(path);
    KERNDB_EXPECT(reopened.ok());
    const auto indexes = reopened.value().Load();
    KERNDB_EXPECT(indexes.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, indexes.value().size());
    KERNDB_EXPECT_EQ(updated.id, indexes.value()[0].id);
    KERNDB_EXPECT_EQ(updated.name, indexes.value()[0].name);
    KERNDB_EXPECT_EQ(updated.table_id, indexes.value()[0].table_id);
    KERNDB_EXPECT_EQ(updated.column_index, indexes.value()[0].column_index);
    KERNDB_EXPECT_EQ(updated.root_page_id, indexes.value()[0].root_page_id);
}
