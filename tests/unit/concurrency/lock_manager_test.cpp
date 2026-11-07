#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "kerndb/concurrency/lock_manager.h"
#include "kerndb/transaction/transaction_manager.h"
#include "test_framework.h"

namespace {

using kerndb::TableId;
using kerndb::concurrency::LockManager;
using kerndb::concurrency::LockMode;
using kerndb::concurrency::LockResource;
using kerndb::transaction::Transaction;
using kerndb::transaction::TransactionManager;

bool WaitUntil(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

KERNDB_TEST(LockManagerAllowsSharedReadersAndBlocksExclusiveRequests) {
    TransactionManager transactions;
    LockManager locks;
    const auto first = transactions.Begin();
    const auto second = transactions.Begin();
    KERNDB_EXPECT(first.ok());
    KERNDB_EXPECT(second.ok());
    const LockResource table = LockResource::Table(TableId{1U});
    KERNDB_EXPECT(locks.Acquire(*first.value(), table, LockMode::kShared).ok());
    KERNDB_EXPECT(locks.Acquire(*second.value(), table, LockMode::kShared).ok());

    const auto writer = transactions.Begin();
    KERNDB_EXPECT(writer.ok());
    std::atomic_bool entered{false};
    std::atomic_bool completed{false};
    kerndb::Status writer_status;
    std::thread thread([&] {
        entered.store(true, std::memory_order_release);
        writer_status = locks.Acquire(*writer.value(), table, LockMode::kExclusive);
        completed.store(true, std::memory_order_release);
    });
    const bool entered_before_timeout = WaitUntil([&] { return entered.load(std::memory_order_acquire); });
    const bool queued_before_release = WaitUntil([&] { return locks.WaitingCount(table) == 1U; });
    const bool completed_before_release = completed.load(std::memory_order_acquire);
    locks.ReleaseAll(first.value()->id());
    locks.ReleaseAll(second.value()->id());
    thread.join();
    KERNDB_EXPECT(entered_before_timeout);
    KERNDB_EXPECT(queued_before_release);
    KERNDB_EXPECT(!completed_before_release);
    KERNDB_EXPECT(writer_status.ok());
    KERNDB_EXPECT(completed.load(std::memory_order_acquire));
    locks.ReleaseAll(writer.value()->id());
}

KERNDB_TEST(LockManagerBlocksSharedRequestsBehindExclusiveHolders) {
    TransactionManager transactions;
    LockManager locks;
    const auto writer = transactions.Begin();
    const auto reader = transactions.Begin();
    KERNDB_EXPECT(writer.ok());
    KERNDB_EXPECT(reader.ok());
    const LockResource table = LockResource::Table(TableId{2U});
    KERNDB_EXPECT(locks.Acquire(*writer.value(), table, LockMode::kExclusive).ok());

    std::atomic_bool completed{false};
    kerndb::Status reader_status;
    std::thread thread([&] {
        reader_status = locks.Acquire(*reader.value(), table, LockMode::kShared);
        completed.store(true, std::memory_order_release);
    });
    const bool queued_before_release = WaitUntil([&] { return locks.WaitingCount(table) == 1U; });
    const bool completed_before_release = completed.load(std::memory_order_acquire);
    locks.ReleaseAll(writer.value()->id());
    thread.join();
    KERNDB_EXPECT(queued_before_release);
    KERNDB_EXPECT(!completed_before_release);
    KERNDB_EXPECT(reader_status.ok());
    locks.ReleaseAll(reader.value()->id());
}

KERNDB_TEST(LockManagerDetectsUpgradeDeadlockAndWakesTheOtherWaiter) {
    TransactionManager transactions;
    LockManager locks;
    const auto first = transactions.Begin();
    const auto second = transactions.Begin();
    KERNDB_EXPECT(first.ok());
    KERNDB_EXPECT(second.ok());
    const LockResource first_table = LockResource::Table(TableId{10U});
    const LockResource second_table = LockResource::Table(TableId{11U});
    KERNDB_EXPECT(locks.Acquire(*first.value(), first_table, LockMode::kExclusive).ok());
    KERNDB_EXPECT(locks.Acquire(*second.value(), second_table, LockMode::kExclusive).ok());

    std::atomic_size_t ready{0U};
    std::atomic_bool start{false};
    kerndb::Status first_wait_status;
    kerndb::Status second_wait_status;
    const auto request_after_barrier = [&ready, &start](auto request) {
        ready.fetch_add(1U, std::memory_order_acq_rel);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return request();
    };
    std::thread first_waiter([&] {
        first_wait_status = request_after_barrier([&] {
            return locks.Acquire(*first.value(), second_table, LockMode::kExclusive);
        });
        if (first_wait_status.code() == kerndb::ErrorCode::kDeadlock) {
            static_cast<void>(transactions.Abort(first.value()));
            locks.ReleaseAll(first.value()->id());
        }
    });
    std::thread second_waiter([&] {
        second_wait_status = request_after_barrier([&] {
            return locks.Acquire(*second.value(), first_table, LockMode::kExclusive);
        });
        if (second_wait_status.code() == kerndb::ErrorCode::kDeadlock) {
            static_cast<void>(transactions.Abort(second.value()));
            locks.ReleaseAll(second.value()->id());
        }
    });
    const bool both_ready = WaitUntil([&] { return ready.load(std::memory_order_acquire) == 2U; });
    start.store(true, std::memory_order_release);
    first_waiter.join();
    second_waiter.join();

    KERNDB_EXPECT(both_ready);
    const std::size_t deadlock_count =
        static_cast<std::size_t>(first_wait_status.code() == kerndb::ErrorCode::kDeadlock) +
        static_cast<std::size_t>(second_wait_status.code() == kerndb::ErrorCode::kDeadlock);
    KERNDB_EXPECT_EQ(std::size_t{1U}, deadlock_count);
    const bool first_was_victim = first_wait_status.code() == kerndb::ErrorCode::kDeadlock;
    const kerndb::Status& surviving_status = first_was_victim ? second_wait_status : first_wait_status;
    KERNDB_EXPECT(surviving_status.ok());
    KERNDB_EXPECT_EQ(std::size_t{0U}, locks.WaitingCount(second_table));
    locks.ReleaseAll(first.value()->id());
    locks.ReleaseAll(second.value()->id());
}

KERNDB_TEST(LockManagerReleasesWaitersWhenTheirTransactionIsAborted) {
    TransactionManager transactions;
    LockManager locks;
    const auto holder = transactions.Begin();
    const auto waiter = transactions.Begin();
    KERNDB_EXPECT(holder.ok());
    KERNDB_EXPECT(waiter.ok());
    const LockResource table = LockResource::Table(TableId{20U});
    KERNDB_EXPECT(locks.Acquire(*holder.value(), table, LockMode::kExclusive).ok());

    kerndb::Status wait_status;
    std::thread thread([&] {
        wait_status = locks.Acquire(*waiter.value(), table, LockMode::kShared);
    });
    const bool queued_before_abort = WaitUntil([&] { return locks.WaitingCount(table) == 1U; });
    const kerndb::Status abort_status = transactions.Abort(waiter.value());
    locks.ReleaseAll(waiter.value()->id());
    thread.join();
    KERNDB_EXPECT(queued_before_abort);
    KERNDB_EXPECT(abort_status.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kTransactionAborted, wait_status.code());
    locks.ReleaseAll(holder.value()->id());
}
