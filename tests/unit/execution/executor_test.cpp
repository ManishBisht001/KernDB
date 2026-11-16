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

KERNDB_TEST(ExecutorSortsStablyAndAppliesLimitAfterOrderBy) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE scores (id INT, score INT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO scores VALUES (1, 10)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO scores VALUES (2, 20)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO scores VALUES (3, 20)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO scores VALUES (4, 10)").ok());

    const auto ordered = Execute(catalog, "SELECT id, score FROM scores ORDER BY score DESC, id ASC LIMIT 3");
    KERNDB_EXPECT(ordered.ok());
    KERNDB_EXPECT_EQ(std::size_t{3U}, ordered.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{2}, std::get<std::int64_t>(ordered.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::int64_t{3}, std::get<std::int64_t>(ordered.value().rows[1][0]));
    KERNDB_EXPECT_EQ(std::int64_t{1}, std::get<std::int64_t>(ordered.value().rows[2][0]));

    const auto none = Execute(catalog, "SELECT id FROM scores ORDER BY id LIMIT 0");
    KERNDB_EXPECT(none.ok());
    KERNDB_EXPECT(none.value().rows.empty());
    const auto filtered = Execute(catalog, "SELECT id FROM scores WHERE id >= 3 ORDER BY id DESC");
    KERNDB_EXPECT(filtered.ok());
    KERNDB_EXPECT_EQ(std::int64_t{4}, std::get<std::int64_t>(filtered.value().rows[0][0]));
}

KERNDB_TEST(ExecutorRunsInnerAndLeftNestedLoopJoinsWithNullExtension) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE people (id INT, name TEXT)").ok());
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE visits (person_id INT, score INT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (1, 'Ada')").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (2, 'Grace')").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO people VALUES (3, 'Noah')").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (1, 5)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (1, 15)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (2, 10)").ok());

    const auto inner = Execute(catalog,
        "SELECT people.name, visits.score FROM people INNER JOIN visits "
        "ON people.id = visits.person_id WHERE visits.score >= 10 ORDER BY visits.score DESC");
    KERNDB_EXPECT(inner.ok());
    KERNDB_EXPECT_EQ(std::size_t{2U}, inner.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(inner.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::int64_t{15}, std::get<std::int64_t>(inner.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::string("Grace"), std::get<std::string>(inner.value().rows[1][0]));

    const auto left = Execute(catalog,
        "SELECT people.name, visits.score FROM people LEFT JOIN visits "
        "ON people.id = visits.person_id ORDER BY people.id ASC, visits.score DESC");
    KERNDB_EXPECT(left.ok());
    KERNDB_EXPECT_EQ(std::size_t{4U}, left.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{15}, std::get<std::int64_t>(left.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::string("Noah"), std::get<std::string>(left.value().rows[3][0]));
    KERNDB_EXPECT(std::holds_alternative<std::monostate>(left.value().rows[3][1]));

    const auto grouped = Execute(catalog,
        "SELECT people.id, COUNT(visits.score) FROM people LEFT JOIN visits "
        "ON people.id = visits.person_id GROUP BY people.id ORDER BY people.id ASC");
    KERNDB_EXPECT(grouped.ok());
    KERNDB_EXPECT_EQ(std::size_t{3U}, grouped.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{2}, std::get<std::int64_t>(grouped.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::int64_t{0}, std::get<std::int64_t>(grouped.value().rows[2][1]));

    const auto null_group = Execute(catalog,
        "SELECT visits.person_id, COUNT(*) FROM people LEFT JOIN visits "
        "ON people.id = visits.person_id GROUP BY visits.person_id ORDER BY visits.person_id");
    KERNDB_EXPECT(null_group.ok());
    KERNDB_EXPECT_EQ(std::size_t{3U}, null_group.value().rows.size());
    KERNDB_EXPECT(std::holds_alternative<std::monostate>(null_group.value().rows[2][0]));

    const auto text_aggregates = Execute(catalog, "SELECT COUNT(name), MIN(name), MAX(name) FROM people");
    KERNDB_EXPECT(text_aggregates.ok());
    KERNDB_EXPECT_EQ(std::int64_t{3}, std::get<std::int64_t>(text_aggregates.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(text_aggregates.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::string("Noah"), std::get<std::string>(text_aggregates.value().rows[0][2]));
}

KERNDB_TEST(ExecutorComputesAggregatesGroupsHavingAndEmptyInputSemantics) {
    kerndb::InMemoryCatalog catalog;
    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE visits (person_id INT, score INT)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (1, 5)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (1, 15)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO visits VALUES (2, 10)").ok());

    const auto aggregates = Execute(catalog,
        "SELECT COUNT(*), COUNT(score), SUM(score), AVG(score), MIN(score), MAX(score) FROM visits");
    KERNDB_EXPECT(aggregates.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, aggregates.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{3}, std::get<std::int64_t>(aggregates.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::int64_t{30}, std::get<std::int64_t>(aggregates.value().rows[0][2]));
    KERNDB_EXPECT_EQ(kerndb::ColumnType::kReal, aggregates.value().columns[3].type);
    KERNDB_EXPECT_EQ(10.0, std::get<double>(aggregates.value().rows[0][3]));
    KERNDB_EXPECT_EQ(std::int64_t{5}, std::get<std::int64_t>(aggregates.value().rows[0][4]));
    KERNDB_EXPECT_EQ(std::int64_t{15}, std::get<std::int64_t>(aggregates.value().rows[0][5]));

    const auto grouped = Execute(catalog,
        "SELECT person_id, COUNT(*), SUM(score) FROM visits GROUP BY person_id "
        "HAVING SUM(score) > 10 ORDER BY person_id DESC");
    KERNDB_EXPECT(grouped.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, grouped.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{1}, std::get<std::int64_t>(grouped.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::int64_t{20}, std::get<std::int64_t>(grouped.value().rows[0][2]));

    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE empty_rows (value INT)").ok());
    const auto empty = Execute(catalog,
        "SELECT COUNT(*), SUM(value), AVG(value), MIN(value), MAX(value) FROM empty_rows");
    KERNDB_EXPECT(empty.ok());
    KERNDB_EXPECT_EQ(std::int64_t{0}, std::get<std::int64_t>(empty.value().rows[0][0]));
    for (std::size_t column = 1U; column < 5U; ++column) {
        KERNDB_EXPECT(std::holds_alternative<std::monostate>(empty.value().rows[0][column]));
    }
    const auto no_groups = Execute(catalog, "SELECT value, COUNT(*) FROM empty_rows GROUP BY value");
    KERNDB_EXPECT(no_groups.ok());
    KERNDB_EXPECT(no_groups.value().rows.empty());

    KERNDB_EXPECT(Execute(catalog, "CREATE TABLE overflow_rows (value INT)").ok());
    KERNDB_EXPECT(Execute(catalog,
        "INSERT INTO overflow_rows VALUES (9223372036854775807)").ok());
    KERNDB_EXPECT(Execute(catalog, "INSERT INTO overflow_rows VALUES (1)").ok());
    const auto overflow = Execute(catalog, "SELECT SUM(value) FROM overflow_rows");
    KERNDB_EXPECT(!overflow.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kResourceExhausted, overflow.status().code());
}
