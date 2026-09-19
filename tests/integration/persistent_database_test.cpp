#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "kerndb/database.h"
#include "kerndb/result.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(PersistentDatabaseReopensTheApprovedVerticalSlice) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "people_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
    }

    KERNDB_EXPECT(std::filesystem::exists(database_path / "database.meta"));
    KERNDB_EXPECT(std::filesystem::exists(database_path / "catalog.dat"));
    KERNDB_EXPECT(std::filesystem::exists(database_path / "tables" / "1.dat"));
    KERNDB_EXPECT(std::filesystem::is_directory(database_path / "indexes"));
    KERNDB_EXPECT(std::filesystem::is_directory(database_path / "wal"));
    KERNDB_EXPECT(std::filesystem::is_directory(database_path / "tmp"));

    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        const auto selected = database.Execute("SELECT id, name FROM people WHERE id = 1;");
        KERNDB_EXPECT(selected.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, selected.value().rows.size());
        KERNDB_EXPECT_EQ(std::int64_t{1}, std::get<std::int64_t>(selected.value().rows[0][0]));
        KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(selected.value().rows[0][1]));
    }
}

KERNDB_TEST(PersistentDatabaseReportsACatalogReferencedMissingTableFile) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "missing_table_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT);").ok());
    }

    KERNDB_EXPECT(std::filesystem::remove(database_path / "tables" / "1.dat"));
    const auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(!reopened.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, reopened.status().code());
}

KERNDB_TEST(PersistentDatabaseRetainsMultipleRowsAndPhaseOneSemanticsAfterRestart) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "restart_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 'Manish');").ok());
        KERNDB_EXPECT_EQ(
            kerndb::ErrorCode::kType,
            database.Execute("INSERT INTO people VALUES ('bad', 'type');").status().code());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto ada = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    const auto manish = database.Execute("SELECT id, name FROM people WHERE id = 2;");
    KERNDB_EXPECT(ada.ok());
    KERNDB_EXPECT(manish.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, ada.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, manish.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(ada.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::string("Manish"), std::get<std::string>(manish.value().rows[0][1]));
}

KERNDB_TEST(PersistentDatabaseBuildsMaintainsAndReopensAnIntIndex) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "indexed_people_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());

        const auto create_index = database.Execute("CREATE INDEX people_id_idx ON people(id);");
        KERNDB_EXPECT(create_index.ok());
        KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kCreateIndex, create_index.value().kind);
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 'Manish');").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (3, 'Rahul');").ok());

        const auto found = database.Execute("SELECT id, name FROM people WHERE id = 2;");
        KERNDB_EXPECT(found.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, found.value().rows.size());
        KERNDB_EXPECT_EQ(std::int64_t{2}, std::get<std::int64_t>(found.value().rows[0][0]));
        KERNDB_EXPECT_EQ(std::string("Manish"), std::get<std::string>(found.value().rows[0][1]));

        const auto missing = database.Execute("SELECT id, name FROM people WHERE id = 99;");
        KERNDB_EXPECT(missing.ok());
        KERNDB_EXPECT_EQ(std::size_t{0U}, missing.value().rows.size());
        const auto duplicate = database.Execute("INSERT INTO people VALUES (2, 'Duplicate');");
        KERNDB_EXPECT(!duplicate.ok());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, duplicate.status().code());
    }

    KERNDB_EXPECT(std::filesystem::exists(database_path / "indexes" / "catalog.dat"));
    KERNDB_EXPECT(std::filesystem::exists(database_path / "indexes" / "1.dat"));
    {
        auto reopened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(reopened.ok());
        auto database = std::move(reopened).value();
        const auto found = database.Execute("SELECT id, name FROM people WHERE id = 2;");
        KERNDB_EXPECT(found.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, found.value().rows.size());
        KERNDB_EXPECT_EQ(std::string("Manish"), std::get<std::string>(found.value().rows[0][1]));
        const auto missing = database.Execute("SELECT id, name FROM people WHERE id = 99;");
        KERNDB_EXPECT(missing.ok());
        KERNDB_EXPECT_EQ(std::size_t{0U}, missing.value().rows.size());

        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (4, 'Grace');").ok());
        const auto inserted_after_reopen = database.Execute(
            "SELECT id, name FROM people WHERE id = 4;");
        KERNDB_EXPECT(inserted_after_reopen.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, inserted_after_reopen.value().rows.size());
        KERNDB_EXPECT_EQ(
            std::string("Grace"),
            std::get<std::string>(inserted_after_reopen.value().rows[0][1]));
    }
}

KERNDB_TEST(PersistentDatabaseCreatesAnIndexOverAnEmptyTable) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "empty_index_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());

        const auto missing = database.Execute("SELECT id, name FROM people WHERE id = 1;");
        KERNDB_EXPECT(missing.ok());
        KERNDB_EXPECT_EQ(std::size_t{0U}, missing.value().rows.size());

        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto found = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(found.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, found.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(found.value().rows[0][1]));
}

KERNDB_TEST(PersistentDatabaseIndexesExistingRowsAcrossSplitsAndRestart) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "split_index_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        for (std::int64_t id = 96; id >= 1; --id) {
            const std::string insert =
                "INSERT INTO people VALUES (" + std::to_string(id) + ", 'person');";
            KERNDB_EXPECT(database.Execute(insert).ok());
        }

        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());
        for (const std::int64_t id : {std::int64_t{1}, std::int64_t{48}, std::int64_t{96}}) {
            const std::string select =
                "SELECT id, name FROM people WHERE id = " + std::to_string(id) + ";";
            const auto found = database.Execute(select);
            KERNDB_EXPECT(found.ok());
            KERNDB_EXPECT_EQ(std::size_t{1U}, found.value().rows.size());
            KERNDB_EXPECT_EQ(id, std::get<std::int64_t>(found.value().rows[0][0]));
        }
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    for (const std::int64_t id : {std::int64_t{1}, std::int64_t{48}, std::int64_t{96}}) {
        const std::string select =
            "SELECT id, name FROM people WHERE id = " + std::to_string(id) + ";";
        const auto found = database.Execute(select);
        KERNDB_EXPECT(found.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, found.value().rows.size());
        KERNDB_EXPECT_EQ(id, std::get<std::int64_t>(found.value().rows[0][0]));
    }
}

KERNDB_TEST(PersistentDatabaseRejectsAChecksumCorruptedTablePageOnScan) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "corrupt_page_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
    }

    const std::filesystem::path table_path = database_path / "tables" / "1.dat";
    std::fstream file(table_path, std::ios::binary | std::ios::in | std::ios::out);
    KERNDB_EXPECT(file.is_open());
    file.seekp(100);
    file.put(static_cast<char>(0xA5));
    file.flush();
    KERNDB_EXPECT(file.good());

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto selected = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(!selected.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, selected.status().code());
}
