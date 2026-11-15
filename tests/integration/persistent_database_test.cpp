#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "kerndb/database.h"
#include "kerndb/result.h"
#include "kerndb/storage/bplus_tree.h"
#include "kerndb/storage/page_manager.h"
#include "catalog/catalog.h"
#include "storage/persistent_storage.h"
#include "test_framework.h"
#include "temp_directory.h"

namespace {

[[nodiscard]] kerndb::Result<kerndb::RecordId> ReadPersistentIndexRecordId(
    const std::filesystem::path& database_path,
    std::string_view table_name,
    std::size_t column_index,
    std::int64_t key) {
    kerndb::InMemoryCatalog catalog;
    auto opened_storage = kerndb::storage::PersistentStorage::Open(database_path, catalog);
    if (!opened_storage.ok()) {
        return opened_storage.status();
    }
    const auto table = catalog.FindTableByName(table_name);
    if (!table.ok()) {
        return table.status();
    }
    const auto index = catalog.FindIndexForTableColumn(table.value()->id, column_index);
    if (!index.ok()) {
        return index.status();
    }
    const kerndb::IndexId index_id = index.value()->id;
    const kerndb::PageId root_page_id = index.value()->root_page_id;
    opened_storage.value().reset();

    auto pages = kerndb::storage::PageManager::Open(
        database_path / "indexes" / (kerndb::ToString(index_id) + ".dat"),
        false);
    if (!pages.ok()) {
        return pages.status();
    }
    auto tree = kerndb::storage::BPlusTree::Open(pages.value(), root_page_id);
    if (!tree.ok()) {
        return tree.status();
    }
    return tree.value().Search(key);
}

}  // namespace

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

KERNDB_TEST(PersistentOptimizerExplainsAndUsesAnAvailableEqualityIndex) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "indexed_queries";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE values_table (id INT, label TEXT)").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO values_table VALUES (1, 'one')").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO values_table VALUES (2, 'two')").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX values_id_idx ON values_table(id)").ok());

        const auto explain = database.Execute("EXPLAIN SELECT label FROM values_table WHERE id = 2");
        KERNDB_EXPECT(explain.ok());
        std::string plan;
        for (const auto& row : explain.value().rows) {
            plan += std::get<std::string>(row[0]);
            plan += "\n";
        }
        KERNDB_EXPECT(plan.find("Index Scan [values_table]") != std::string::npos);

        const auto selected = database.Execute(
            "SELECT label FROM values_table WHERE id = 2 ORDER BY label LIMIT 1");
        KERNDB_EXPECT(selected.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, selected.value().rows.size());
        KERNDB_EXPECT_EQ(std::string("two"), std::get<std::string>(selected.value().rows[0][0]));
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

KERNDB_TEST(PersistentDatabaseUpdatesRowsAndMaintainsUniqueIndexes) {
    kerndb::test::TemporaryDirectory directory;
    auto opened = kerndb::Database::Open(directory.path() / "update_database");
    KERNDB_EXPECT(opened.ok());
    auto database = std::move(opened).value();
    KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 'Grace');").ok());
    KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());

    const auto non_indexed_update = database.Execute(
        "UPDATE people SET name = 'Vikas Kumar' WHERE id = 1;");
    KERNDB_EXPECT(non_indexed_update.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kUpdate, non_indexed_update.value().kind);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, non_indexed_update.value().rows_affected);
    const auto renamed = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(renamed.ok());
    KERNDB_EXPECT_EQ(std::string("Vikas Kumar"), std::get<std::string>(renamed.value().rows[0][1]));

    const auto indexed_update = database.Execute("UPDATE people SET id = 4 WHERE id = 1;");
    KERNDB_EXPECT(indexed_update.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, indexed_update.value().rows_affected);
    const auto old_key = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    const auto new_key = database.Execute("SELECT id, name FROM people WHERE id = 4;");
    KERNDB_EXPECT(old_key.ok());
    KERNDB_EXPECT(new_key.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, old_key.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, new_key.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Vikas Kumar"), std::get<std::string>(new_key.value().rows[0][1]));

    const auto duplicate = database.Execute("UPDATE people SET id = 2 WHERE id = 4;");
    KERNDB_EXPECT(!duplicate.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, duplicate.status().code());
    const auto retained = database.Execute("SELECT id, name FROM people WHERE id = 4;");
    const auto existing = database.Execute("SELECT id, name FROM people WHERE id = 2;");
    KERNDB_EXPECT(retained.ok());
    KERNDB_EXPECT(existing.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, retained.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, existing.value().rows.size());
}

KERNDB_TEST(PersistentDatabaseDeletesRowsMaintainsIndexesAndReusesKeys) {
    kerndb::test::TemporaryDirectory directory;
    auto opened = kerndb::Database::Open(directory.path() / "delete_database");
    KERNDB_EXPECT(opened.ok());
    auto database = std::move(opened).value();
    KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 'Grace');").ok());
    KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());

    const auto deleted = database.Execute("DELETE FROM people WHERE id = 1;");
    KERNDB_EXPECT(deleted.ok());
    KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kDelete, deleted.value().kind);
    KERNDB_EXPECT_EQ(std::uint64_t{1U}, deleted.value().rows_affected);
    const auto missing = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(missing.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, missing.value().rows.size());

    const auto nonexistent = database.Execute("DELETE FROM people WHERE id = 99;");
    KERNDB_EXPECT(nonexistent.ok());
    KERNDB_EXPECT_EQ(std::uint64_t{0U}, nonexistent.value().rows_affected);
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Reused');").ok());
    const auto reused = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    KERNDB_EXPECT(reused.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, reused.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Reused"), std::get<std::string>(reused.value().rows[0][1]));
}

KERNDB_TEST(PersistentDatabaseMutationChangesSurviveRestartWithMultipleIndexes) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "multiple_index_mutation_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, employee_code INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 10, 'Ada');").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 20, 'Grace');").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_code_idx ON people(employee_code);").ok());
        KERNDB_EXPECT(database.Execute(
            "UPDATE people SET employee_code = 11 WHERE id = 1;").ok());
        KERNDB_EXPECT(database.Execute("DELETE FROM people WHERE id = 2;").ok());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto updated_id = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    const auto updated_code = database.Execute(
        "SELECT id, name FROM people WHERE employee_code = 11;");
    const auto old_code = database.Execute("SELECT id FROM people WHERE employee_code = 10;");
    const auto deleted_id = database.Execute("SELECT id FROM people WHERE id = 2;");
    const auto deleted_code = database.Execute("SELECT id FROM people WHERE employee_code = 20;");
    KERNDB_EXPECT(updated_id.ok());
    KERNDB_EXPECT(updated_code.ok());
    KERNDB_EXPECT(old_code.ok());
    KERNDB_EXPECT(deleted_id.ok());
    KERNDB_EXPECT(deleted_code.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, updated_id.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, updated_code.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{0U}, old_code.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{0U}, deleted_id.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{0U}, deleted_code.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(updated_code.value().rows[0][1]));
}

KERNDB_TEST(PersistentStorageRollsBackAnUpdateAfterHeapAndIndexChanges) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "rollback_database";

    kerndb::InMemoryCatalog catalog;
    auto opened_storage = kerndb::storage::PersistentStorage::Open(database_path, catalog);
    KERNDB_EXPECT(opened_storage.ok());
    auto storage = std::move(opened_storage).value();

    const kerndb::Schema schema{
        {.name = "id", .type = kerndb::ColumnType::kInt},
        {.name = "name", .type = kerndb::ColumnType::kText},
    };
    const auto table_id = catalog.CreateTable("people", schema);
    KERNDB_EXPECT(table_id.ok());
    auto table_result = catalog.FindTableById(table_id.value());
    KERNDB_EXPECT(table_result.ok());
    kerndb::InMemoryTable* table = table_result.value();
    KERNDB_EXPECT(storage->CreateTable(*table).ok());

    const auto index_id = catalog.CreateIndex("people_id_idx", table_id.value(), 0U);
    KERNDB_EXPECT(index_id.ok());
    const auto index = catalog.FindIndexById(index_id.value());
    KERNDB_EXPECT(index.ok());
    const auto root_page_id = storage->CreateIndex(*index.value());
    KERNDB_EXPECT(root_page_id.ok());
    KERNDB_EXPECT(catalog.UpdateIndexRoot(index_id.value(), root_page_id.value()).ok());
    const auto inserted_record = storage->Insert(
        *table,
        kerndb::Tuple{std::int64_t{4}, std::string("Original")});
    KERNDB_EXPECT(inserted_record.ok());

    KERNDB_EXPECT(storage->BeginTransaction().ok());
    KERNDB_EXPECT(storage->Insert(
        *table,
        kerndb::Tuple{std::int64_t{6}, std::string("Pending")}).ok());
    storage->FailNextUpdateAfterRowMutationForTesting();
    const auto failed_update = storage->UpdateMatching(
        *table,
        0U,
        kerndb::Value{std::int64_t{5}},
        0U,
        kerndb::Value{std::int64_t{4}});
    KERNDB_EXPECT(!failed_update.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kIo, failed_update.status().code());

    const auto expect_original_state = [](
        const kerndb::storage::PersistentStorage& persistent_storage,
        const kerndb::InMemoryTable& persistent_table) {
        const auto rows = persistent_storage.Scan(persistent_table);
        KERNDB_EXPECT(rows.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, rows.value().size());
        KERNDB_EXPECT_EQ(std::int64_t{4}, std::get<std::int64_t>(rows.value()[0][0]));
        KERNDB_EXPECT_EQ(std::string("Original"), std::get<std::string>(rows.value()[0][1]));

        const auto old_key = persistent_storage.LookupByIndex(persistent_table, 0U, 4);
        KERNDB_EXPECT(old_key.ok());
        KERNDB_EXPECT(old_key.value().index_available);
        KERNDB_EXPECT(old_key.value().tuple.has_value());
        KERNDB_EXPECT_EQ(std::int64_t{4}, std::get<std::int64_t>((*old_key.value().tuple)[0]));
        KERNDB_EXPECT_EQ(std::string("Original"), std::get<std::string>((*old_key.value().tuple)[1]));

        const auto new_key = persistent_storage.LookupByIndex(persistent_table, 0U, 5);
        KERNDB_EXPECT(new_key.ok());
        KERNDB_EXPECT(new_key.value().index_available);
        KERNDB_EXPECT(!new_key.value().tuple.has_value());
    };
    const auto expect_original_rid = [
        &database_path,
        &inserted_record,
        &index_id](kerndb::PageId index_root_page_id) {
        auto index_pages = kerndb::storage::PageManager::Open(
            database_path / "indexes" / (kerndb::ToString(index_id.value()) + ".dat"),
            false);
        KERNDB_EXPECT(index_pages.ok());
        auto tree = kerndb::storage::BPlusTree::Open(index_pages.value(), index_root_page_id);
        KERNDB_EXPECT(tree.ok());
        const auto restored_rid = tree.value().Search(4);
        KERNDB_EXPECT(restored_rid.ok());
        KERNDB_EXPECT_EQ(inserted_record.value(), restored_rid.value());
        const auto removed_new_key = tree.value().Search(5);
        KERNDB_EXPECT(!removed_new_key.ok());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, removed_new_key.status().code());
    };

    // The injected failure occurs after replacement insert, old-key removal,
    // new-key insertion, and old-row tombstoning. Rollback must therefore run
    // Restore(old row), Remove(new key), Insert(old key), Erase(replacement).
    const auto rows_after_failed_statement = storage->Scan(*table);
    KERNDB_EXPECT(rows_after_failed_statement.ok());
    KERNDB_EXPECT_EQ(std::size_t{2U}, rows_after_failed_statement.value().size());
    const auto original_key_after_failure = storage->LookupByIndex(*table, 0U, 4);
    const auto failed_new_key = storage->LookupByIndex(*table, 0U, 5);
    const auto earlier_transaction_key = storage->LookupByIndex(*table, 0U, 6);
    KERNDB_EXPECT(original_key_after_failure.ok());
    KERNDB_EXPECT(original_key_after_failure.value().tuple.has_value());
    KERNDB_EXPECT(failed_new_key.ok());
    KERNDB_EXPECT(!failed_new_key.value().tuple.has_value());
    KERNDB_EXPECT(earlier_transaction_key.ok());
    KERNDB_EXPECT(earlier_transaction_key.value().tuple.has_value());
    KERNDB_EXPECT_EQ(std::string("Original"),
        std::get<std::string>((*original_key_after_failure.value().tuple)[1]));
    KERNDB_EXPECT_EQ(std::string("Pending"),
        std::get<std::string>((*earlier_transaction_key.value().tuple)[1]));

    // The transaction remains active and its earlier successful insert is
    // still undoable after the failed statement compensates its own work.
    KERNDB_EXPECT(storage->RollbackTransaction().ok());
    expect_original_state(*storage, *table);

    storage.reset();
    kerndb::InMemoryCatalog reopened_catalog;
    auto reopened_storage_result = kerndb::storage::PersistentStorage::Open(
        database_path,
        reopened_catalog);
    KERNDB_EXPECT(reopened_storage_result.ok());
    auto reopened_storage = std::move(reopened_storage_result).value();
    const auto reopened_table = reopened_catalog.FindTableByName("people");
    KERNDB_EXPECT(reopened_table.ok());
    expect_original_state(*reopened_storage, *reopened_table.value());
    const auto reopened_index = reopened_catalog.FindIndexById(index_id.value());
    KERNDB_EXPECT(reopened_index.ok());
    const kerndb::PageId reopened_root_page_id = reopened_index.value()->root_page_id;
    reopened_storage.reset();
    expect_original_rid(reopened_root_page_id);
}

KERNDB_TEST(PersistentDatabaseCommitsExplicitInsertAndPersistsIt) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "explicit_commit_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());

        const auto begin = database.Execute("BEGIN;");
        KERNDB_EXPECT(begin.ok());
        KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kBegin, begin.value().kind);
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (10, 'Committed');").ok());

        const auto visible_inside = database.Execute("SELECT id, name FROM people WHERE id = 10;");
        KERNDB_EXPECT(visible_inside.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, visible_inside.value().rows.size());

        const auto commit = database.Execute("COMMIT;");
        KERNDB_EXPECT(commit.ok());
        KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kCommit, commit.value().kind);
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto selected = database.Execute("SELECT id, name FROM people WHERE id = 10;");
    KERNDB_EXPECT(selected.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, selected.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Committed"), std::get<std::string>(selected.value().rows[0][1]));
}

KERNDB_TEST(PersistentDatabaseRollsBackExplicitInsertAcrossReopen) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "explicit_rollback_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("BEGIN;").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (11, 'Rolled back');").ok());

        const auto visible_inside = database.Execute("SELECT id FROM people WHERE id = 11;");
        KERNDB_EXPECT(visible_inside.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, visible_inside.value().rows.size());

        const auto rollback = database.Execute("ROLLBACK;");
        KERNDB_EXPECT(rollback.ok());
        KERNDB_EXPECT_EQ(kerndb::QueryResultKind::kRollback, rollback.value().kind);
        const auto absent_after_rollback = database.Execute("SELECT id FROM people WHERE id = 11;");
        KERNDB_EXPECT(absent_after_rollback.ok());
        KERNDB_EXPECT_EQ(std::size_t{0U}, absent_after_rollback.value().rows.size());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto absent_after_reopen = database.Execute("SELECT id FROM people WHERE id = 11;");
    KERNDB_EXPECT(absent_after_reopen.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, absent_after_reopen.value().rows.size());
}

KERNDB_TEST(PersistentDatabaseRollsBackMultipleMutationsInReverseAndRestoresIndexedRid) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "multi_undo_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Original');").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());
    }
    const auto original_rid = ReadPersistentIndexRecordId(database_path, "people", 0U, 1);
    KERNDB_EXPECT(original_rid.ok());

    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();

        KERNDB_EXPECT(database.Execute("BEGIN;").ok());
        KERNDB_EXPECT(database.Execute("UPDATE people SET id = 2 WHERE id = 1;").ok());
        KERNDB_EXPECT(database.Execute("UPDATE people SET name = 'Changed' WHERE id = 2;").ok());
        const auto own_update = database.Execute("SELECT id, name FROM people WHERE id = 2;");
        KERNDB_EXPECT(own_update.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, own_update.value().rows.size());
        KERNDB_EXPECT_EQ(std::string("Changed"), std::get<std::string>(own_update.value().rows[0][1]));
        KERNDB_EXPECT(database.Execute("DELETE FROM people WHERE id = 2;").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (3, 'Temporary');").ok());
        KERNDB_EXPECT(database.Execute("ROLLBACK;").ok());

        const auto original_after_rollback = database.Execute("SELECT id, name FROM people WHERE id = 1;");
        const auto moved_key_after_rollback = database.Execute("SELECT id FROM people WHERE id = 2;");
        const auto inserted_key_after_rollback = database.Execute("SELECT id FROM people WHERE id = 3;");
        KERNDB_EXPECT(original_after_rollback.ok());
        KERNDB_EXPECT(moved_key_after_rollback.ok());
        KERNDB_EXPECT(inserted_key_after_rollback.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, original_after_rollback.value().rows.size());
        KERNDB_EXPECT_EQ(std::string("Original"),
            std::get<std::string>(original_after_rollback.value().rows[0][1]));
        KERNDB_EXPECT_EQ(std::size_t{0U}, moved_key_after_rollback.value().rows.size());
        KERNDB_EXPECT_EQ(std::size_t{0U}, inserted_key_after_rollback.value().rows.size());

        KERNDB_EXPECT(database.Execute("BEGIN;").ok());
        KERNDB_EXPECT(database.Execute("DELETE FROM people WHERE id = 1;").ok());
        KERNDB_EXPECT(database.Execute("ROLLBACK;").ok());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto original_after_reopen = database.Execute("SELECT id, name FROM people WHERE id = 1;");
    const auto missing_updated_key = database.Execute("SELECT id FROM people WHERE id = 2;");
    const auto missing_inserted_key = database.Execute("SELECT id FROM people WHERE id = 3;");
    KERNDB_EXPECT(original_after_reopen.ok());
    KERNDB_EXPECT(missing_updated_key.ok());
    KERNDB_EXPECT(missing_inserted_key.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, original_after_reopen.value().rows.size());
    KERNDB_EXPECT_EQ(std::string("Original"),
        std::get<std::string>(original_after_reopen.value().rows[0][1]));
    KERNDB_EXPECT_EQ(std::size_t{0U}, missing_updated_key.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{0U}, missing_inserted_key.value().rows.size());
    const auto restored_rid = ReadPersistentIndexRecordId(database_path, "people", 0U, 1);
    KERNDB_EXPECT(restored_rid.ok());
    KERNDB_EXPECT_EQ(original_rid.value(), restored_rid.value());
    const auto removed_key_rid = ReadPersistentIndexRecordId(database_path, "people", 0U, 2);
    KERNDB_EXPECT(!removed_key_rid.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kNotFound, removed_key_rid.status().code());
}

KERNDB_TEST(PersistentDatabaseKeepsFailedStatementAtomicAndTransactionActive) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "statement_error_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (1, 'Ada');").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (2, 'Grace');").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());

        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kInvalidArgument,
            database.Execute("COMMIT;").status().code());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kInvalidArgument,
            database.Execute("ROLLBACK;").status().code());

        KERNDB_EXPECT(database.Execute("BEGIN;").ok());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kInvalidArgument,
            database.Execute("BEGIN;").status().code());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kUnsupported,
            database.Execute("CREATE TABLE rejected (id INT);").status().code());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kUnsupported,
            database.Execute("CREATE INDEX rejected_idx ON people(id);").status().code());

        const auto failed_update = database.Execute("UPDATE people SET id = 1 WHERE id = 2;");
        KERNDB_EXPECT(!failed_update.ok());
        KERNDB_EXPECT_EQ(kerndb::ErrorCode::kAlreadyExists, failed_update.status().code());
        const auto retained_one = database.Execute("SELECT id, name FROM people WHERE id = 1;");
        const auto retained_two = database.Execute("SELECT id, name FROM people WHERE id = 2;");
        KERNDB_EXPECT(retained_one.ok());
        KERNDB_EXPECT(retained_two.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, retained_one.value().rows.size());
        KERNDB_EXPECT_EQ(std::size_t{1U}, retained_two.value().rows.size());
        KERNDB_EXPECT_EQ(std::string("Ada"), std::get<std::string>(retained_one.value().rows[0][1]));
        KERNDB_EXPECT_EQ(std::string("Grace"), std::get<std::string>(retained_two.value().rows[0][1]));

        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (3, 'Lin');").ok());
        const auto visible_insert = database.Execute("SELECT name FROM people WHERE id = 3;");
        KERNDB_EXPECT(visible_insert.ok());
        KERNDB_EXPECT_EQ(std::size_t{1U}, visible_insert.value().rows.size());
        KERNDB_EXPECT(database.Execute("COMMIT;").ok());

        // Outside an explicit transaction, this INSERT autocommits as before.
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (4, 'Ken');").ok());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto committed = database.Execute("SELECT name FROM people WHERE id = 3;");
    const auto autocommitted = database.Execute("SELECT name FROM people WHERE id = 4;");
    KERNDB_EXPECT(committed.ok());
    KERNDB_EXPECT(autocommitted.ok());
    KERNDB_EXPECT_EQ(std::string("Lin"), std::get<std::string>(committed.value().rows[0][0]));
    KERNDB_EXPECT_EQ(std::string("Ken"), std::get<std::string>(autocommitted.value().rows[0][0]));
}

KERNDB_TEST(PersistentDatabaseCloseRollsBackActiveTransaction) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "active_close_database";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
    }
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("BEGIN;").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (20, 'Uncommitted');").ok());
    }

    auto reopened = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(reopened.ok());
    auto database = std::move(reopened).value();
    const auto selected = database.Execute("SELECT id FROM people WHERE id = 20;");
    KERNDB_EXPECT(selected.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, selected.value().rows.size());
}

KERNDB_TEST(InMemoryDatabaseSupportsExplicitTransactionRollback) {
    kerndb::Database database;
    KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
    KERNDB_EXPECT(database.Execute("BEGIN;").ok());
    KERNDB_EXPECT(database.Execute("INSERT INTO people VALUES (30, 'Memory');").ok());
    const auto visible = database.Execute("SELECT name FROM people WHERE id = 30;");
    KERNDB_EXPECT(visible.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, visible.value().rows.size());
    KERNDB_EXPECT(database.Execute("ROLLBACK;").ok());
    const auto absent = database.Execute("SELECT id FROM people WHERE id = 30;");
    KERNDB_EXPECT(absent.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, absent.value().rows.size());
}
