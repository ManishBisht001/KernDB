#include <string>
#include <variant>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "kerndb/types.h"
#include "parser/parser.h"
#include "test_framework.h"

namespace {

kerndb::InMemoryCatalog MakeCatalog() {
    kerndb::InMemoryCatalog catalog;
    const kerndb::Schema schema{
        kerndb::ColumnDefinition{.name = "id", .type = kerndb::ColumnType::kInt},
        kerndb::ColumnDefinition{.name = "name", .type = kerndb::ColumnType::kText},
    };
    const auto create = catalog.CreateTable("people", schema);
    if (!create.ok()) {
        throw std::runtime_error(create.status().ToString());
    }
    return catalog;
}

}  // namespace

KERNDB_TEST(BinderResolvesCaseInsensitiveTablesAndColumns) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();
    const auto ast = kerndb::parser::ParseSql("SELECT ID, Name FROM PEOPLE WHERE id = 1");
    KERNDB_EXPECT(ast.ok());

    const auto bound = kerndb::binder::BindStatement(ast.value(), catalog);
    KERNDB_EXPECT(bound.ok());
    const auto& select = std::get<kerndb::binder::BoundSelect>(bound.value());
    KERNDB_EXPECT_EQ(std::size_t{2U}, select.projection_indices.size());
    KERNDB_EXPECT(select.predicate.has_value());
}

KERNDB_TEST(BinderRejectsDuplicateColumnsAndInvalidDataReferences) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();

    const auto duplicate_columns = kerndb::parser::ParseSql(
        "CREATE TABLE duplicate_columns (id INT, ID TEXT)");
    KERNDB_EXPECT(duplicate_columns.ok());
    const auto duplicate_bound = kerndb::binder::BindStatement(duplicate_columns.value(), catalog);
    KERNDB_EXPECT(!duplicate_bound.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kBinding, duplicate_bound.status().code());

    const auto missing_table = kerndb::parser::ParseSql("SELECT id FROM absent");
    KERNDB_EXPECT(missing_table.ok());
    const auto missing_table_bound = kerndb::binder::BindStatement(missing_table.value(), catalog);
    KERNDB_EXPECT(!missing_table_bound.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kBinding, missing_table_bound.status().code());

    const auto missing_column = kerndb::parser::ParseSql("SELECT age FROM people");
    KERNDB_EXPECT(missing_column.ok());
    const auto missing_column_bound = kerndb::binder::BindStatement(missing_column.value(), catalog);
    KERNDB_EXPECT(!missing_column_bound.ok());

    const auto wrong_count = kerndb::parser::ParseSql("INSERT INTO people VALUES (1)");
    KERNDB_EXPECT(wrong_count.ok());
    KERNDB_EXPECT(!kerndb::binder::BindStatement(wrong_count.value(), catalog).ok());

    const auto wrong_insert_type = kerndb::parser::ParseSql(
        "INSERT INTO people VALUES ('one', 'Ada')");
    KERNDB_EXPECT(wrong_insert_type.ok());
    const auto wrong_insert_bound = kerndb::binder::BindStatement(wrong_insert_type.value(), catalog);
    KERNDB_EXPECT(!wrong_insert_bound.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kType, wrong_insert_bound.status().code());

    const auto wrong_where_type = kerndb::parser::ParseSql(
        "SELECT id FROM people WHERE id = 'one'");
    KERNDB_EXPECT(wrong_where_type.ok());
    const auto wrong_where_bound = kerndb::binder::BindStatement(wrong_where_type.value(), catalog);
    KERNDB_EXPECT(!wrong_where_bound.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kType, wrong_where_bound.status().code());
}
