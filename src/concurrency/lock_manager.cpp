#include "kerndb/concurrency/lock_manager.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace kerndb::concurrency {

Status LockManager::Acquire(
    const transaction::Transaction& transaction,
    LockResource resource,
    LockMode mode) {
    std::unique_lock lock(mutex_);
    if (transaction.state() != transaction::TransactionState::kActive) {
        return Status::Error(ErrorCode::kTransactionAborted, "transaction is not active");
    }

    LockState& state = lock_table_[resource];
    const auto held = state.holders.find(transaction.id().value());
    if (held != state.holders.end()) {
        if (held->second == LockMode::kExclusive || mode == LockMode::kShared) {
            return Status::Ok();
        }
    }

    if (state.waiters.empty() && CanGrant(state, transaction.id(), mode)) {
        state.holders[transaction.id().value()] = mode;
        return Status::Ok();
    }

    const auto duplicate_wait = std::find_if(
        state.waiters.begin(),
        state.waiters.end(),
        [&transaction](const WaitRequest& request) {
            return request.transaction_id == transaction.id();
        });
    if (duplicate_wait != state.waiters.end()) {
        return Status::Error(ErrorCode::kInvalidArgument, "transaction already waits for this lock");
    }

    state.waiters.push_back(WaitRequest{transaction.id(), mode});
    if (RequestCreatesCycle(transaction.id())) {
        RemoveWaiter(state, transaction.id());
        if (state.holders.empty() && state.waiters.empty()) {
            lock_table_.erase(resource);
        }
        condition_.notify_all();
        return Status::Error(ErrorCode::kDeadlock, "deadlock detected; requesting transaction is the victim")
            .WithContext("transaction_id", ToString(transaction.id()));
    }

    condition_.wait(lock, [&transaction, resource, mode, this] {
        if (transaction.state() != transaction::TransactionState::kActive) {
            return true;
        }
        const auto found = lock_table_.find(resource);
        return found == lock_table_.end() || CanGrant(found->second, transaction.id(), mode);
    });

    if (transaction.state() != transaction::TransactionState::kActive) {
        return Status::Error(ErrorCode::kTransactionAborted, "transaction was aborted while waiting for a lock")
            .WithContext("transaction_id", ToString(transaction.id()));
    }

    const auto current = lock_table_.find(resource);
    if (current == lock_table_.end()) {
        return Status::Error(ErrorCode::kInternal, "lock wait state disappeared before grant");
    }
    RemoveWaiter(current->second, transaction.id());
    current->second.holders[transaction.id().value()] = mode;
    condition_.notify_all();
    return Status::Ok();
}

void LockManager::ReleaseAll(TransactionId transaction_id) {
    std::scoped_lock lock(mutex_);
    for (auto iterator = lock_table_.begin(); iterator != lock_table_.end();) {
        LockState& state = iterator->second;
        state.holders.erase(transaction_id.value());
        RemoveWaiter(state, transaction_id);
        if (state.holders.empty() && state.waiters.empty()) {
            iterator = lock_table_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    condition_.notify_all();
}

std::size_t LockManager::WaitingCount(LockResource resource) const {
    std::scoped_lock lock(mutex_);
    const auto found = lock_table_.find(resource);
    return found == lock_table_.end() ? 0U : found->second.waiters.size();
}

bool LockManager::Compatible(LockMode held, LockMode requested) noexcept {
    return held == LockMode::kShared && requested == LockMode::kShared;
}

bool LockManager::CanGrant(
    const LockState& state,
    TransactionId transaction_id,
    LockMode mode) {
    for (const auto& [holder_id, held_mode] : state.holders) {
        if (holder_id != transaction_id.value() && !Compatible(held_mode, mode)) {
            return false;
        }
    }

    for (const WaitRequest& request : state.waiters) {
        if (request.transaction_id == transaction_id) {
            return true;
        }
        // Shared requests may join a preceding shared cohort, but no request
        // may bypass an earlier exclusive waiter.
        if (mode == LockMode::kExclusive || request.mode == LockMode::kExclusive) {
            return false;
        }
    }
    return true;
}

bool LockManager::RequestCreatesCycle(TransactionId requester) const {
    std::map<std::uint64_t, std::set<std::uint64_t>> wait_for;
    for (const auto& [resource, state] : lock_table_) {
        static_cast<void>(resource);
        std::vector<WaitRequest> earlier_waiters;
        for (const WaitRequest& waiter : state.waiters) {
            auto& blockers = wait_for[waiter.transaction_id.value()];
            for (const auto& [holder_id, held_mode] : state.holders) {
                if (holder_id != waiter.transaction_id.value() && !Compatible(held_mode, waiter.mode)) {
                    blockers.insert(holder_id);
                }
            }
            for (const WaitRequest& earlier : earlier_waiters) {
                if (waiter.mode == LockMode::kExclusive || earlier.mode == LockMode::kExclusive) {
                    blockers.insert(earlier.transaction_id.value());
                }
            }
            earlier_waiters.push_back(waiter);
        }
    }

    const std::uint64_t start = requester.value();
    std::set<std::uint64_t> visited;
    std::function<bool(std::uint64_t)> reaches_start = [&](std::uint64_t current) {
        const auto found = wait_for.find(current);
        if (found == wait_for.end()) {
            return false;
        }
        for (const std::uint64_t next : found->second) {
            if (next == start) {
                return true;
            }
            if (visited.insert(next).second && reaches_start(next)) {
                return true;
            }
        }
        return false;
    };
    visited.insert(start);
    return reaches_start(start);
}

void LockManager::RemoveWaiter(LockState& state, TransactionId transaction_id) {
    std::erase_if(state.waiters, [transaction_id](const WaitRequest& request) {
        return request.transaction_id == transaction_id;
    });
}

}  // namespace kerndb::concurrency
