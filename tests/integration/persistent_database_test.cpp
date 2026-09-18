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
