#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/transaction/transaction_manager.h"

namespace kerndb::concurrency {

enum class LockMode : std::uint8_t {
    kShared,
    kExclusive,
};

enum class LockResourceKind : std::uint8_t {
    kCatalog,
    kTable,
};

struct LockResource {
    LockResourceKind kind{LockResourceKind::kCatalog};
    std::uint64_t id{0U};

    [[nodiscard]] static constexpr LockResource Catalog() noexcept {
        return LockResource{LockResourceKind::kCatalog, 0U};
    }

    [[nodiscard]] static constexpr LockResource Table(TableId table_id) noexcept {
        return LockResource{LockResourceKind::kTable, table_id.value()};
    }

    [[nodiscard]] constexpr auto operator<=>(const LockResource&) const = default;
};

// FIFO S/X lock table. A wait-for graph is rebuilt from the live holders and
// wait queues when a request blocks; the requesting transaction is the
// deterministic deadlock victim and its session performs storage rollback.
class LockManager {
public:
    [[nodiscard]] Status Acquire(
        const transaction::Transaction& transaction,
        LockResource resource,
        LockMode mode);
    void ReleaseAll(TransactionId transaction_id);

    // Introspection used by deterministic lock-manager tests and diagnostics.
    [[nodiscard]] std::size_t WaitingCount(LockResource resource) const;

private:
    struct WaitRequest {
        TransactionId transaction_id;
        LockMode mode{LockMode::kShared};
    };

    struct LockState {
        std::map<std::uint64_t, LockMode> holders;
        std::deque<WaitRequest> waiters;
    };

    [[nodiscard]] static bool Compatible(LockMode held, LockMode requested) noexcept;
    [[nodiscard]] static bool CanGrant(
        const LockState& state,
        TransactionId transaction_id,
        LockMode mode);
    [[nodiscard]] bool RequestCreatesCycle(TransactionId requester) const;
    void RemoveWaiter(LockState& state, TransactionId transaction_id);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::map<LockResource, LockState> lock_table_;
};

}  // namespace kerndb::concurrency
