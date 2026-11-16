#include <cstdint>
#include <string>
#include <variant>

#include "parser/ast.h"
#include "parser/parser.h"
#include "test_framework.h"

KERNDB_TEST(ParserBuildsAllSupportedStatementKinds) {
    const auto create = kerndb::parser::ParseSql("CREATE TABLE People (id INT, name TEXT);");
    KERNDB_EXPECT(create.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstCreateTable>(create.value()));

    const auto create_index = kerndb::parser::ParseSql(
        "CREATE INDEX people_id_idx ON people(id);");
    KERNDB_EXPECT(create_index.ok());
    const auto& index_statement = std::get<kerndb::parser::AstCreateIndex>(create_index.value());
    KERNDB_EXPECT_EQ(std::string("people_id_idx"), index_statement.index_name.text);
    KERNDB_EXPECT_EQ(std::string("people"), index_statement.table_name.text);
    KERNDB_EXPECT_EQ(std::string("id"), index_statement.column_name.text);

    const auto insert = kerndb::parser::ParseSql("INSERT INTO people VALUES (-1, 'Ada''s')");
    KERNDB_EXPECT(insert.ok());
    const auto& insert_statement = std::get<kerndb::parser::AstInsert>(insert.value());
    KERNDB_EXPECT_EQ(std::int64_t{-1}, std::get<std::int64_t>(insert_statement.values[0].value));
    KERNDB_EXPECT_EQ(std::string("Ada's"), std::get<std::string>(insert_statement.values[1].value));

    const auto select = kerndb::parser::ParseSql(
        "SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(select.ok());
    const auto& select_statement = std::get<kerndb::parser::AstSelect>(select.value());
    KERNDB_EXPECT_EQ(std::size_t{2U}, select_statement.projections.size());
    KERNDB_EXPECT(select_statement.predicate.has_value());

    const auto update = kerndb::parser::ParseSql(
        "UPDATE people SET name = 'Vikas Kumar' WHERE id = 4;");
    KERNDB_EXPECT(update.ok());
    const auto& update_statement = std::get<kerndb::parser::AstUpdate>(update.value());
    KERNDB_EXPECT_EQ(std::string("name"), update_statement.column_name.text);
    KERNDB_EXPECT_EQ(std::int64_t{4}, std::get<std::int64_t>(update_statement.predicate.literal.value));

    const auto delete_statement = kerndb::parser::ParseSql("DELETE FROM people WHERE id = 4;");
    KERNDB_EXPECT(delete_statement.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstDelete>(delete_statement.value()));

    const auto begin = kerndb::parser::ParseSql("BEGIN;");
    KERNDB_EXPECT(begin.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstBegin>(begin.value()));

    const auto commit = kerndb::parser::ParseSql("COMMIT");
    KERNDB_EXPECT(commit.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstCommit>(commit.value()));

    const auto rollback = kerndb::parser::ParseSql("ROLLBACK;");
    KERNDB_EXPECT(rollback.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstRollback>(rollback.value()));
}

KERNDB_TEST(ParserRejectsUnsupportedAndMultiStatementSyntax) {
    const auto star = kerndb::parser::ParseSql("SELECT * FROM people");
    KERNDB_EXPECT(!star.ok());

    const auto multiple = kerndb::parser::ParseSql(
        "CREATE TABLE people (id INT); INSERT INTO people VALUES (1);");
    KERNDB_EXPECT(!multiple.ok());

    const auto malformed = kerndb::parser::ParseSql("CREATE TABLE people (id)");
    KERNDB_EXPECT(!malformed.ok());

    const auto unsafe_update = kerndb::parser::ParseSql("UPDATE people SET id = 4");
    KERNDB_EXPECT(!unsafe_update.ok());

    KERNDB_EXPECT(!kerndb::parser::ParseSql("BEGIN TRANSACTION").ok());
    KERNDB_EXPECT(!kerndb::parser::ParseSql("COMMIT AND CHAIN").ok());
}

KERNDB_TEST(ParserBuildsAdvancedSelectClausesAndExplain) {
    const auto parsed = kerndb::parser::ParseSql(
        "SELECT people.id, COUNT(*) FROM people LEFT JOIN visits "
        "ON people.id = visits.person_id WHERE people.id >= 1 "
        "GROUP BY people.id HAVING COUNT(*) > 0 "
        "ORDER BY people.id DESC, visits.person_id ASC LIMIT 5");
    KERNDB_EXPECT(parsed.ok());
    const auto& select = std::get<kerndb::parser::AstSelect>(parsed.value());
    KERNDB_EXPECT_EQ(std::size_t{2U}, select.projections.size());
    KERNDB_EXPECT(select.join.has_value());
    KERNDB_EXPECT_EQ(kerndb::parser::AstJoinKind::kLeft, select.join->kind);
    KERNDB_EXPECT_EQ(kerndb::parser::ComparisonOperator::kGreaterEqual, select.predicate->comparison);
    KERNDB_EXPECT_EQ(std::size_t{1U}, select.group_by.size());
    KERNDB_EXPECT(select.having.has_value());
    KERNDB_EXPECT_EQ(std::size_t{2U}, select.order_by.size());
    KERNDB_EXPECT_EQ(std::int64_t{5}, select.limit.value());

    const auto explain = kerndb::parser::ParseSql("EXPLAIN SELECT id FROM people ORDER BY id LIMIT 0");
    KERNDB_EXPECT(explain.ok());
    KERNDB_EXPECT(std::holds_alternative<kerndb::parser::AstExplainSelect>(explain.value()));
}
