#include <cstdint>
#include <string>

#include "catalog/catalog.h"
#include "kerndb/types.h"
#include "test_framework.h"

KERNDB_TEST(CatalogUsesCaseInsensitiveNamesAndStableTableIds) {
    kerndb::InMemoryCatalog catalog;
    const kerndb::Schema schema{
        kerndb::ColumnDefinition{.name = "id", .type = kerndb::ColumnType::kInt},
    };

    const auto table_id = catalog.CreateTable("People", schema);
    KERNDB_EXPECT(table_id.ok());
    KERNDB_EXPECT(table_id.value().valid());
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, table_id.value().value());

    const auto table = catalog.FindTableByName("PEOPLE");
    KERNDB_EXPECT(table.ok());
    KERNDB_EXPECT_EQ(std::string("people"), table.value()->name);
    KERNDB_EXPECT_EQ(table_id.value(), table.value()->id);
}

KERNDB_TEST(CatalogRejectsDuplicateTables) {
    kerndb::InMemoryCatalog catalog;
    const kerndb::Schema schema{
        kerndb::ColumnDefinition{.name = "id", .type = kerndb::ColumnType::kInt},
    };

    KERNDB_EXPECT(catalog.CreateTable("people", schema).ok());
    const auto duplicate = catalog.CreateTable("People", schema);
    KERNDB_EXPECT(!duplicate.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, duplicate.status().code());
}
