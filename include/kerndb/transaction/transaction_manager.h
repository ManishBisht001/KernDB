#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "kerndb/ids.h"
#include "kerndb/result.h"

namespace kerndb::transaction {

enum class TransactionState : std::uint8_t {
    kActive,
    kCommitted,
    kAborted,
};

class Transaction {
public:
    [[nodiscard]] TransactionId id() const noexcept;
    [[nodiscard]] TransactionState state() const noexcept;

private:
    friend class TransactionManager;

    explicit Transaction(TransactionId id) noexcept;

    TransactionId id_;
    std::atomic<TransactionState> state_{TransactionState::kActive};
};

// Allocates IDs and owns the lifecycle of independent transaction contexts.
// Lock ownership lives in LockManager; storage rollback is initiated by the
// session that owns each Transaction.
class TransactionManager {
public:
    [[nodiscard]] Result<std::shared_ptr<Transaction>> Begin();
    [[nodiscard]] Status Commit(const std::shared_ptr<Transaction>& transaction);
    [[nodiscard]] Status Abort(const std::shared_ptr<Transaction>& transaction);
    [[nodiscard]] std::size_t active_count() const;

private:
    [[nodiscard]] Status Finish(
        const std::shared_ptr<Transaction>& transaction,
        TransactionState terminal_state);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Transaction>> active_transactions_;
};

}  // namespace kerndb::transaction
