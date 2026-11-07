#include "kerndb/transaction/transaction_manager.h"

#include <utility>

namespace kerndb::transaction {
namespace {

std::atomic<std::uint64_t> g_next_transaction_id{1U};

[[nodiscard]] Result<TransactionId> AllocateTransactionId() {
    std::uint64_t candidate = g_next_transaction_id.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == TransactionId::kInvalidValue) {
            return Status::Error(ErrorCode::kResourceExhausted, "transaction ID space is exhausted");
        }
        if (g_next_transaction_id.compare_exchange_weak(
                candidate,
                candidate + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return TransactionId{candidate};
        }
    }
}

}  // namespace

Transaction::Transaction(TransactionId id) noexcept
    : id_(id) {}

TransactionId Transaction::id() const noexcept {
    return id_;
}

TransactionState Transaction::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

Result<std::shared_ptr<Transaction>> TransactionManager::Begin() {
    const auto allocated_id = AllocateTransactionId();
    if (!allocated_id.ok()) {
        return allocated_id.status();
    }

    std::scoped_lock lock(mutex_);
    const TransactionId id = allocated_id.value();
    auto transaction = std::shared_ptr<Transaction>(new Transaction{id});
    active_transactions_.emplace(id.value(), transaction);
    return transaction;
}

Status TransactionManager::Commit(const std::shared_ptr<Transaction>& transaction) {
    return Finish(transaction, TransactionState::kCommitted);
}

Status TransactionManager::Abort(const std::shared_ptr<Transaction>& transaction) {
    return Finish(transaction, TransactionState::kAborted);
}

std::size_t TransactionManager::active_count() const {
    std::scoped_lock lock(mutex_);
    return active_transactions_.size();
}

Status TransactionManager::Finish(
    const std::shared_ptr<Transaction>& transaction,
    TransactionState terminal_state) {
    if (transaction == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "transaction context is null");
    }

    std::scoped_lock lock(mutex_);
    const auto found = active_transactions_.find(transaction->id().value());
    if (found == active_transactions_.end() || found->second != transaction ||
        transaction->state() != TransactionState::kActive) {
        return Status::Error(ErrorCode::kInvalidArgument, "transaction is not active");
    }

    transaction->state_.store(terminal_state, std::memory_order_release);
    active_transactions_.erase(found);
    return Status::Ok();
}

}  // namespace kerndb::transaction
