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

KERNDB_TEST(BinderResolvesCreateIndexAndRestrictsItToIntColumns) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();
    const auto create_index = kerndb::parser::ParseSql(
        "CREATE INDEX PEOPLE_ID_IDX ON people(ID)");
    KERNDB_EXPECT(create_index.ok());
    const auto bound = kerndb::binder::BindStatement(create_index.value(), catalog);
    KERNDB_EXPECT(bound.ok());
    const auto& index = std::get<kerndb::binder::BoundCreateIndex>(bound.value());
    KERNDB_EXPECT_EQ(std::string("people_id_idx"), index.index_name);
    KERNDB_EXPECT_EQ(std::size_t{0U}, index.column_index);

    const auto text_index = kerndb::parser::ParseSql(
        "CREATE INDEX people_name_idx ON people(name)");
    KERNDB_EXPECT(text_index.ok());
    const auto rejected = kerndb::binder::BindStatement(text_index.value(), catalog);
    KERNDB_EXPECT(!rejected.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kBinding, rejected.status().code());
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

KERNDB_TEST(BinderResolvesUpdateAndDeleteMutations) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();

    const auto update_ast = kerndb::parser::ParseSql(
        "UPDATE people SET name = 'Vikas Kumar' WHERE id = 4");
    KERNDB_EXPECT(update_ast.ok());
    const auto update = kerndb::binder::BindStatement(update_ast.value(), catalog);
    KERNDB_EXPECT(update.ok());
    const auto& bound_update = std::get<kerndb::binder::BoundUpdate>(update.value());
    KERNDB_EXPECT_EQ(std::size_t{1U}, bound_update.column_index);
    KERNDB_EXPECT_EQ(std::size_t{0U}, bound_update.predicate.column_index);

    const auto delete_ast = kerndb::parser::ParseSql("DELETE FROM people WHERE id = 4");
    KERNDB_EXPECT(delete_ast.ok());
    const auto deleted = kerndb::binder::BindStatement(delete_ast.value(), catalog);
    KERNDB_EXPECT(deleted.ok());
    const auto& bound_delete = std::get<kerndb::binder::BoundDelete>(deleted.value());
    KERNDB_EXPECT_EQ(std::size_t{0U}, bound_delete.predicate.column_index);

    const auto mismatched_update = kerndb::parser::ParseSql(
        "UPDATE people SET id = 'four' WHERE id = 4");
    KERNDB_EXPECT(mismatched_update.ok());
    const auto rejected = kerndb::binder::BindStatement(mismatched_update.value(), catalog);
    KERNDB_EXPECT(!rejected.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kType, rejected.status().code());
}

KERNDB_TEST(BinderResolvesQualifiedJoinColumnsAndRejectsAmbiguity) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();
    KERNDB_EXPECT(catalog.CreateTable("visits", {
        {.name = "id", .type = kerndb::ColumnType::kInt},
        {.name = "person_id", .type = kerndb::ColumnType::kInt},
        {.name = "score", .type = kerndb::ColumnType::kInt},
    }).ok());

    const auto qualified = kerndb::parser::ParseSql(
        "SELECT people.name, visits.score FROM people INNER JOIN visits "
        "ON visits.person_id = people.id ORDER BY visits.score DESC");
    KERNDB_EXPECT(qualified.ok());
    const auto bound = kerndb::binder::BindStatement(qualified.value(), catalog);
    KERNDB_EXPECT(bound.ok());
    const auto& select = std::get<kerndb::binder::BoundSelect>(bound.value());
    KERNDB_EXPECT(select.join.has_value());
    KERNDB_EXPECT_EQ(std::size_t{0U}, select.join->left_column.relation_index);

    const auto ambiguous = kerndb::parser::ParseSql(
        "SELECT id FROM people JOIN visits ON people.id = visits.person_id");
    KERNDB_EXPECT(ambiguous.ok());
    const auto rejected = kerndb::binder::BindStatement(ambiguous.value(), catalog);
    KERNDB_EXPECT(!rejected.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kBinding, rejected.status().code());
}

KERNDB_TEST(BinderValidatesGroupingAggregateTypesAndLimit) {
    kerndb::InMemoryCatalog catalog = MakeCatalog();
    const auto wrong_aggregate = kerndb::parser::ParseSql("SELECT SUM(name) FROM people");
    KERNDB_EXPECT(wrong_aggregate.ok());
    const auto rejected_aggregate = kerndb::binder::BindStatement(wrong_aggregate.value(), catalog);
    KERNDB_EXPECT(!rejected_aggregate.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kType, rejected_aggregate.status().code());

    const auto ungrouped = kerndb::parser::ParseSql("SELECT name, COUNT(*) FROM people GROUP BY id");
    KERNDB_EXPECT(ungrouped.ok());
    const auto rejected_column = kerndb::binder::BindStatement(ungrouped.value(), catalog);
    KERNDB_EXPECT(!rejected_column.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kBinding, rejected_column.status().code());

    const auto negative_limit = kerndb::parser::ParseSql("SELECT id FROM people LIMIT -1");
    KERNDB_EXPECT(negative_limit.ok());
    const auto rejected_limit = kerndb::binder::BindStatement(negative_limit.value(), catalog);
    KERNDB_EXPECT(!rejected_limit.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kInvalidArgument, rejected_limit.status().code());
}
