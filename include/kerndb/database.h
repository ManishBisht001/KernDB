#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>

#include "kerndb/result.h"
#include "kerndb/types.h"

namespace kerndb {

class InMemoryCatalog;
struct DatabaseRuntime;
namespace transaction {
class Transaction;
}
namespace storage {
class TableStorage;
}

struct EngineIdentity {
    std::string_view name;
    std::string_view version;
};

[[nodiscard]] constexpr EngineIdentity GetEngineIdentity() noexcept {
    return EngineIdentity{
        .name = "KernDB",
        .version = "0.0.0",
    };
}

class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    [[nodiscard]] static Result<Database> Open(const std::filesystem::path& database_directory);
    [[nodiscard]] Result<QueryResult> Execute(std::string_view sql);

private:
    [[nodiscard]] Result<QueryResult> BeginTransaction();
    [[nodiscard]] Result<QueryResult> CommitTransaction();
    [[nodiscard]] Result<QueryResult> RollbackTransaction();
    [[nodiscard]] Status RollbackActiveTransaction();
    [[nodiscard]] Status FinishImplicitTransaction(
        const std::shared_ptr<transaction::Transaction>& transaction,
        bool succeeded,
        const Status* statement_error = nullptr);

    explicit Database(std::shared_ptr<DatabaseRuntime> runtime);

    std::shared_ptr<DatabaseRuntime> runtime_;
    std::unique_ptr<InMemoryCatalog> in_memory_transaction_snapshot_;
    std::shared_ptr<transaction::Transaction> active_transaction_;
    bool explicit_transaction_active_{false};
    mutable std::mutex session_mutex_;
};

}  // namespace kerndb
