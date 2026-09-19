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
}

KERNDB_TEST(ParserRejectsUnsupportedAndMultiStatementSyntax) {
    const auto star = kerndb::parser::ParseSql("SELECT * FROM people");
    KERNDB_EXPECT(!star.ok());

    const auto multiple = kerndb::parser::ParseSql(
        "CREATE TABLE people (id INT); INSERT INTO people VALUES (1);");
    KERNDB_EXPECT(!multiple.ok());

    const auto malformed = kerndb::parser::ParseSql("CREATE TABLE people (id)");
    KERNDB_EXPECT(!malformed.ok());
}
