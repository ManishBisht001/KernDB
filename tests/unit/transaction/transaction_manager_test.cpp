#include "kerndb/transaction/transaction_manager.h"
#include "test_framework.h"

KERNDB_TEST(TransactionManagerCreatesUniqueContextsAndTracksTerminalStates) {
    kerndb::transaction::TransactionManager manager;
    const auto first = manager.Begin();
    const auto second = manager.Begin();
    kerndb::transaction::TransactionManager other_manager;
    const auto third = other_manager.Begin();
    KERNDB_EXPECT(first.ok());
    KERNDB_EXPECT(second.ok());
    KERNDB_EXPECT(third.ok());
    KERNDB_EXPECT(first.value()->id() != second.value()->id());
    KERNDB_EXPECT(second.value()->id() != third.value()->id());
    KERNDB_EXPECT_EQ(kerndb::transaction::TransactionState::kActive, first.value()->state());
    KERNDB_EXPECT_EQ(std::size_t{2U}, manager.active_count());

    KERNDB_EXPECT(manager.Commit(first.value()).ok());
    KERNDB_EXPECT_EQ(kerndb::transaction::TransactionState::kCommitted, first.value()->state());
    KERNDB_EXPECT(manager.Abort(second.value()).ok());
    KERNDB_EXPECT(other_manager.Abort(third.value()).ok());
    KERNDB_EXPECT_EQ(kerndb::transaction::TransactionState::kAborted, second.value()->state());
    KERNDB_EXPECT_EQ(std::size_t{0U}, manager.active_count());
    KERNDB_EXPECT_EQ(std::size_t{0U}, other_manager.active_count());
    KERNDB_EXPECT_EQ(
        kerndb::ErrorCode::kInvalidArgument,
        manager.Commit(first.value()).code());
}
