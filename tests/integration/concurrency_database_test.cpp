#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <variant>

#include "kerndb/database.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(PersistentSessionsBlockConflictingWritesAndCommitIndependentTransactions) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "concurrent_sessions";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE people (id INT, name TEXT);").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX people_id_idx ON people(id);").ok());
    }

    auto first_open = kerndb::Database::Open(database_path);
    auto second_open = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(first_open.ok());
    KERNDB_EXPECT(second_open.ok());
    auto first = std::move(first_open).value();
    auto second = std::move(second_open).value();
    KERNDB_EXPECT(first.Execute("BEGIN;").ok());
    KERNDB_EXPECT(first.Execute("INSERT INTO people VALUES (1, 'first');").ok());

    std::mutex ready_mutex;
    std::condition_variable ready_condition;
    bool second_ready = false;
    kerndb::Status second_begin;
    kerndb::Status second_insert;
    kerndb::Status second_commit;
    std::thread second_session([&] {
        const auto begin = second.Execute("BEGIN;");
        second_begin = begin.ok() ? kerndb::Status::Ok() : begin.status();
        {
            std::scoped_lock lock(ready_mutex);
            second_ready = true;
        }
        ready_condition.notify_one();
        if (!begin.ok()) {
            return;
        }

        const auto insert = second.Execute("INSERT INTO people VALUES (2, 'second');");
        second_insert = insert.ok() ? kerndb::Status::Ok() : insert.status();
        if (insert.ok()) {
            const auto commit = second.Execute("COMMIT;");
            second_commit = commit.ok() ? kerndb::Status::Ok() : commit.status();
        }
    });

    {
        std::unique_lock lock(ready_mutex);
        static_cast<void>(ready_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return second_ready;
        }));
    }
    const auto first_commit_result = first.Execute("COMMIT;");
    const kerndb::Status first_commit = first_commit_result.ok()
                                            ? kerndb::Status::Ok()
                                            : first_commit_result.status();
    second_session.join();

    KERNDB_EXPECT(second_ready);
    KERNDB_EXPECT(second_begin.ok());
    KERNDB_EXPECT(first_commit.ok());
    KERNDB_EXPECT(second_insert.ok());
    KERNDB_EXPECT(second_commit.ok());

    const auto both_rows = first.Execute("SELECT id, name FROM people;");
    KERNDB_EXPECT(both_rows.ok());
    KERNDB_EXPECT_EQ(std::size_t{2U}, both_rows.value().rows.size());
    const auto indexed_first = second.Execute("SELECT name FROM people WHERE id = 1;");
    const auto indexed_second = second.Execute("SELECT name FROM people WHERE id = 2;");
    KERNDB_EXPECT(indexed_first.ok());
    KERNDB_EXPECT(indexed_second.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, indexed_first.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, indexed_second.value().rows.size());
}

KERNDB_TEST(PersistentSessionsAllowConcurrentReadsAndBlockReadWriteConflicts) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "read_write_sessions";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE values_table (id INT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO values_table VALUES (1);").ok());
    }

    auto first_open = kerndb::Database::Open(database_path);
    auto second_open = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(first_open.ok());
    KERNDB_EXPECT(second_open.ok());
    auto first = std::move(first_open).value();
    auto second = std::move(second_open).value();
    KERNDB_EXPECT(first.Execute("BEGIN;").ok());
    const auto first_read = first.Execute("SELECT id FROM values_table;");
    KERNDB_EXPECT(first_read.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, first_read.value().rows.size());

    std::mutex state_mutex;
    std::condition_variable state_condition;
    bool second_started = false;
    bool second_read_done = false;
    bool second_write_done = false;
    kerndb::Status second_begin;
    kerndb::Status second_read;
    kerndb::Status second_write;
    kerndb::Status second_commit;
    std::thread second_session([&] {
        const auto begin = second.Execute("BEGIN;");
        second_begin = begin.ok() ? kerndb::Status::Ok() : begin.status();
        {
            std::scoped_lock lock(state_mutex);
            second_started = true;
        }
        state_condition.notify_one();
        if (!begin.ok()) {
            return;
        }

        const auto read = second.Execute("SELECT id FROM values_table;");
        second_read = read.ok() ? kerndb::Status::Ok() : read.status();
        {
            std::scoped_lock lock(state_mutex);
            second_read_done = true;
        }
        state_condition.notify_one();
        if (!read.ok()) {
            return;
        }

        const auto insert = second.Execute("INSERT INTO values_table VALUES (2);");
        second_write = insert.ok() ? kerndb::Status::Ok() : insert.status();
        {
            std::scoped_lock lock(state_mutex);
            second_write_done = true;
        }
        state_condition.notify_one();
        if (insert.ok()) {
            const auto commit = second.Execute("COMMIT;");
            second_commit = commit.ok() ? kerndb::Status::Ok() : commit.status();
        }
    });

    bool read_finished_while_first_held_shared_lock = false;
    bool write_finished_before_first_released = false;
    {
        std::unique_lock lock(state_mutex);
        static_cast<void>(state_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return second_started;
        }));
        read_finished_while_first_held_shared_lock = state_condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&] { return second_read_done; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::scoped_lock lock(state_mutex);
        write_finished_before_first_released = second_write_done;
    }
    const auto first_commit = first.Execute("COMMIT;");
    second_session.join();

    KERNDB_EXPECT(second_started);
    KERNDB_EXPECT(read_finished_while_first_held_shared_lock);
    KERNDB_EXPECT(!write_finished_before_first_released);
    KERNDB_EXPECT(first_commit.ok());
    KERNDB_EXPECT(second_begin.ok());
    KERNDB_EXPECT(second_read.ok());
    KERNDB_EXPECT(second_write.ok());
    KERNDB_EXPECT(second_commit.ok());
    const auto rows = first.Execute("SELECT id FROM values_table;");
    KERNDB_EXPECT(rows.ok());
    KERNDB_EXPECT_EQ(std::size_t{2U}, rows.value().rows.size());
}

KERNDB_TEST(PersistentSessionsDetectDeadlockRollbackVictimAndWakeSurvivor) {
    kerndb::test::TemporaryDirectory directory;
    const std::filesystem::path database_path = directory.path() / "deadlock_sessions";
    {
        auto opened = kerndb::Database::Open(database_path);
        KERNDB_EXPECT(opened.ok());
        auto database = std::move(opened).value();
        KERNDB_EXPECT(database.Execute("CREATE TABLE left_rows (id INT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO left_rows VALUES (1);").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX left_rows_id_idx ON left_rows(id);").ok());
        KERNDB_EXPECT(database.Execute("CREATE TABLE right_rows (id INT);").ok());
        KERNDB_EXPECT(database.Execute("INSERT INTO right_rows VALUES (10);").ok());
        KERNDB_EXPECT(database.Execute("CREATE INDEX right_rows_id_idx ON right_rows(id);").ok());
    }

    auto left_open = kerndb::Database::Open(database_path);
    auto right_open = kerndb::Database::Open(database_path);
    KERNDB_EXPECT(left_open.ok());
    KERNDB_EXPECT(right_open.ok());
    auto left = std::move(left_open).value();
    auto right = std::move(right_open).value();
    KERNDB_EXPECT(left.Execute("BEGIN;").ok());
    KERNDB_EXPECT(right.Execute("BEGIN;").ok());
    KERNDB_EXPECT(left.Execute("UPDATE left_rows SET id = 2 WHERE id = 1;").ok());
    KERNDB_EXPECT(right.Execute("UPDATE right_rows SET id = 20 WHERE id = 10;").ok());

    std::mutex start_mutex;
    std::condition_variable start_condition;
    std::size_t ready_count = 0U;
    bool start = false;
    kerndb::Status left_cross_update;
    kerndb::Status right_cross_update;
    const auto await_start = [&] {
        std::unique_lock lock(start_mutex);
        ++ready_count;
        start_condition.notify_one();
        start_condition.wait(lock, [&] { return start; });
    };
    std::thread left_thread([&] {
        await_start();
        const auto result = left.Execute("UPDATE right_rows SET id = 11 WHERE id = 10;");
        left_cross_update = result.ok() ? kerndb::Status::Ok() : result.status();
    });
    std::thread right_thread([&] {
        await_start();
        const auto result = right.Execute("UPDATE left_rows SET id = 12 WHERE id = 1;");
        right_cross_update = result.ok() ? kerndb::Status::Ok() : result.status();
    });
    {
        std::unique_lock lock(start_mutex);
        static_cast<void>(start_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return ready_count == 2U;
        }));
        start = true;
    }
    start_condition.notify_all();
    left_thread.join();
    right_thread.join();

    const bool left_victim = left_cross_update.code() == kerndb::ErrorCode::kDeadlock;
    const bool right_victim = right_cross_update.code() == kerndb::ErrorCode::kDeadlock;
    KERNDB_EXPECT(left_victim != right_victim);
    KERNDB_EXPECT(left_victim ? right_cross_update.ok() : left_cross_update.ok());

    const auto left_commit = left.Execute("COMMIT;");
    const auto right_commit = right.Execute("COMMIT;");
    KERNDB_EXPECT(left_commit.ok() != right_commit.ok());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kInvalidArgument,
        (left_victim ? left_commit.status() : right_commit.status()).code());

    const auto left_rows = left.Execute("SELECT id FROM left_rows;");
    const auto right_rows = right.Execute("SELECT id FROM right_rows;");
    KERNDB_EXPECT(left_rows.ok());
    KERNDB_EXPECT(right_rows.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, left_rows.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, right_rows.value().rows.size());
    if (left_victim) {
        KERNDB_EXPECT_EQ(std::int64_t{12}, std::get<std::int64_t>(left_rows.value().rows[0][0]));
        KERNDB_EXPECT_EQ(std::int64_t{20}, std::get<std::int64_t>(right_rows.value().rows[0][0]));
    } else {
        KERNDB_EXPECT_EQ(std::int64_t{2}, std::get<std::int64_t>(left_rows.value().rows[0][0]));
        KERNDB_EXPECT_EQ(std::int64_t{11}, std::get<std::int64_t>(right_rows.value().rows[0][0]));
    }

    const auto left_indexed = left.Execute(
        left_victim ? "SELECT id FROM left_rows WHERE id = 12;" : "SELECT id FROM left_rows WHERE id = 2;");
    const auto right_indexed = right.Execute(
        left_victim ? "SELECT id FROM right_rows WHERE id = 20;" : "SELECT id FROM right_rows WHERE id = 11;");
    KERNDB_EXPECT(left_indexed.ok());
    KERNDB_EXPECT(right_indexed.ok());
    KERNDB_EXPECT_EQ(std::size_t{1U}, left_indexed.value().rows.size());
    KERNDB_EXPECT_EQ(std::size_t{1U}, right_indexed.value().rows.size());
}
