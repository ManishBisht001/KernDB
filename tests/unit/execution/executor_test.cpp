#include <cstdint>
#include <string>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "execution/execution_context.h"
#include "execution/executor.h"
#include "kerndb/types.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "test_framework.h"

namespace {

kerndb::Result<kerndb::QueryResult> Execute(
    kerndb::InMemoryCatalog& catalog,
    std::string_view sql) {
    const auto ast = kerndb::parser::ParseSql(sql);
    if (!ast.ok()) {
        return ast.status();
    }
    const auto bound = kerndb::binder::BindStatement(ast.value(), catalog);
    if (!bound.ok()) {
        return bound.status();
    }
    const auto plan = kerndb::planner::PlanStatement(bound.value());
    if (!plan.ok()) {
        return plan.status();
    }
    kerndb::execution::ExecutionContext context{
        .catalog = catalog,
        .table_storage = nullptr,
        .transaction_id = std::nullopt,
    };
    return kerndb::execution::ExecutePlan(plan.value(), context);
}

}  // namespace

KERNDB_TEST(ExecutorCreatesInsertsFiltersAndProjectsInMemoryRows) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE people (id INT, name TEXT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (1, 'Ada')").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (2, 'Grace')").ok());

    const auto result = Execute(catalog, "SELECT name FROM people WHERE id = 1");
    KERNDB_EXPECT(result.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kSelect, result.value().kind);
    KERNDB_EXPECT_EQ(std::size_t{1U}, result.value().columns.size());
    KERNDB_EXPECT_EQ(std::string("name"), result.value().columns[0].name);
    KERNDB_EXPECT_EQ(std::size_t{1U}, result.value().rows.size());
    KERNDB_EXPECT_EQ(
        std::string("Ada"),
        std::get<std::string>(result.value().rows[0][0]));
}

KERNDB_TEST(ExecutorAcceptsCreateIndexForTheInMemoryFallback) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE people (id INT, name TEXT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (1, 'Ada')").ok());

    const auto create_index = Execute(catalog, "CREATE INDEX people_id_idx ON people(id)");
    KERNDB_EXPECT(create_index.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kCreateIndex, create_index.value().kind);

    const auto selected = Execute(catalog, "SELECT name FROM people WHERE id = 1");
    KERNDB_EXPECT(selected.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, selected.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(selected.value().rows[0][0]));
}

KERNDB_TEST(ExecutorUpdatesAndDeletesInMemoryRowsAtomically) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE people (id INT, name TEXT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (1, 'Ada')").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (2, 'Grace')").ok());
    KERNDB_EXPECT(Execute(catalog, "CREATE INDEX people_id_idx ON people(id)").ok());

    const auto update = Execute(catalog, "UPDATE people SET name = 'Vikas Kumar' WHERE id = 1");
    KERNDB_EXPECT(update.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kUpdate, update.value().kind);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, update.value().rows_affected);

    const auto duplicate = Execute(catalog, "UPDATE people SET id = 2 WHERE id = 1");
    KERNDB_EXPECT(!duplicate.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, duplicate.status().code());
    const auto retained = Execute(catalog, "SELECT name FROM people WHERE id = 1");
    KERNDB_EXPECT(retained.ok());
    KERNDB_EXPECT_EQ(std::string("Vikas Kumar"), std::get<std::string>(retained.value().rows[0][0]));

    const auto deleted = Execute(catalog, "DELETE FROM people WHERE id = 1");
    KERNDB_EXPECT(deleted.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kDelete, deleted.value().kind);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, deleted.value().rows_affected);
    const auto missing = Execute(catalog, "SELECT id FROM people WHERE id = 1");
    KERNDB_EXPECT(missing.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, missing.value().rows.size());
}
