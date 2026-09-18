#include <cstdint>
#include <string>

#include "kerndb/database.h"
#include "test_framework.h"

KERNDB_TEST(DatabaseExecutesTheApprovedPhaseOneVerticalSlice) {
    kerndb::Database database;

    const auto create = database.Execute("CREATE TABLE people (id INT, name TEXT);");
    KERNDB_EXPECT(create.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kCreateTable, create.value().kind);

    const auto insert = database.Execute("INSERT INTO people VALUES (1, 'Ada');");
    KERNDB_EXPECT(insert.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, insert.value().rows_affected);

    const auto select = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(select.ok());
    KERNDB_EXPECT_EQ(std::size_t{2U}, select.value().columns.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, select.value().rows.size());
    KERNDB_EXPECT_EQ(std::int64_t{1}, std::get<std::int64_t>(select.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(select.value().rows[0][1]));
}

KERNDB_TEST(DatabaseReportsTheRequiredPhaseOneErrors) {
    kerndb::Database database;
    KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT)").ok());

    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kAlreadyExists,
        database.Execute("CREATE TABLE People (id INT)").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kBinding,
        database.Execute("CREATE TABLE duplicate_column (id INT, ID TEXT)").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kBinding,
        database.Execute("INSERT INTO missing VALUES (1)").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kBinding,
        database.Execute("SELECT missing FROM people").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kBinding,
        database.Execute("INSERT INTO people VALUES (1)").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kType,
        database.Execute("INSERT INTO people VALUES ('one', 'Ada')").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kBinding,
        database.Execute("SELECT id FROM people WHERE missing = 1").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kType,
        database.Execute("SELECT id FROM people WHERE id = 'one'").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kSyntax,
        database.Execute("SELECT FROM people").status().code());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kSyntax,
        database.Execute("INSERT INTO people VALUES ('Ada)").status().code());
}
