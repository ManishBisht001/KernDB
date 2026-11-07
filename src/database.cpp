#include "kerndb/database.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "execution/execution_context.h"
#include "execution/executor.h"
#include "kerndb/concurrency/lock_manager.h"
#include "kerndb/transaction/transaction_manager.h"
#include "parser/parser.h"
#include "planner/physical_plan.h"
#include "planner/planner.h"
#include "storage/persistent_storage.h"

namespace kerndb {

struct DatabaseRuntime {
    std::shared_ptr<InMemoryCatalog> catalog;
    std::shared_ptr<storage::TableStorage> table_storage;
    std::shared_ptr<transaction::TransactionManager> transaction_manager;
    std::shared_ptr<concurrency::LockManager> lock_manager;
    std::filesystem::path database_directory;
};

namespace {

constexpr ErrorCode kCatalogOpenError = ErrorCode::kIo;
std::mutex g_runtime_registry_mutex;
std::map<std::string, std::weak_ptr<DatabaseRuntime>, std::less<>> g_runtime_registry;

[[nodiscard]] QueryResult TransactionControlResult(QueryResultKind kind) {
    return QueryResult{
        .kind = kind,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<std::pair<std::filesystem::path, std::string>> NormalizeDatabasePath(
    const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::absolute(path, error);
    if (error) {
        return Status::Error(kCatalogOpenError, "failed to resolve database directory")
            .WithContext("path", path.string())
            .WithContext("error", error.message());
    }
    normalized = normalized.lexically_normal();
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, error);
    if (!error) {
        normalized = canonical;
    }

    std::string key = normalized.generic_string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return std::pair{std::move(normalized), std::move(key)};
}

[[nodiscard]] Result<std::shared_ptr<DatabaseRuntime>> OpenPersistentRuntime(
    const std::filesystem::path& directory) {
    const auto normalized = NormalizeDatabasePath(directory);
    if (!normalized.ok()) {
        return normalized.status();
    }

    std::scoped_lock lock(g_runtime_registry_mutex);
    for (auto iterator = g_runtime_registry.begin(); iterator != g_runtime_registry.end();) {
        if (iterator->second.expired()) {
            iterator = g_runtime_registry.erase(iterator);
        } else {
            ++iterator;
        }
    }
    const auto existing = g_runtime_registry.find(normalized.value().second);
    if (existing != g_runtime_registry.end()) {
        if (auto runtime = existing->second.lock()) {
            return runtime;
        }
    }

    auto catalog = std::make_shared<InMemoryCatalog>();
    auto opened_storage = storage::OpenPersistentStorage(normalized.value().first, *catalog);
    if (!opened_storage.ok()) {
        return opened_storage.status();
    }
    std::shared_ptr<storage::TableStorage> table_storage = std::move(opened_storage).value();
    auto runtime = std::make_shared<DatabaseRuntime>(DatabaseRuntime{
        .catalog = std::move(catalog),
        .table_storage = std::move(table_storage),
        .transaction_manager = std::make_shared<transaction::TransactionManager>(),
        .lock_manager = std::make_shared<concurrency::LockManager>(),
        .database_directory = normalized.value().first,
    });
    g_runtime_registry[normalized.value().second] = runtime;
    return runtime;
}

[[nodiscard]] std::shared_ptr<DatabaseRuntime> CreateInMemoryRuntime() {
    return std::make_shared<DatabaseRuntime>(DatabaseRuntime{
        .catalog = std::make_shared<InMemoryCatalog>(),
        .table_storage = nullptr,
        .transaction_manager = std::make_shared<transaction::TransactionManager>(),
        .lock_manager = std::make_shared<concurrency::LockManager>(),
        .database_directory = {},
    });
}

[[nodiscard]] bool IsDdl(const parser::AstStatement& statement) {
    return std::holds_alternative<parser::AstCreateTable>(statement) ||
           std::holds_alternative<parser::AstCreateIndex>(statement);
}

[[nodiscard]] std::optional<std::pair<TableId, concurrency::LockMode>> LockTarget(
    const planner::PhysicalPlan& plan) {
    return std::visit(
        [](const auto& physical_plan)
            -> std::optional<std::pair<TableId, concurrency::LockMode>> {
            using PlanType = std::decay_t<decltype(physical_plan)>;
            if constexpr (requires { physical_plan.table_id; }) {
                concurrency::LockMode mode = concurrency::LockMode::kShared;
                if constexpr (std::is_same_v<PlanType, planner::PhysicalCreateIndex> ||
                              std::is_same_v<PlanType, planner::PhysicalInsert> ||
                              std::is_same_v<PlanType, planner::PhysicalUpdate> ||
                              std::is_same_v<PlanType, planner::PhysicalDelete>) {
                    mode = concurrency::LockMode::kExclusive;
                }
                return std::pair{physical_plan.table_id, mode};
            }
            return std::nullopt;
        },
        plan);
}

[[nodiscard]] bool IsExplicitMutation(const planner::PhysicalPlan& plan) {
    return std::visit(
        [](const auto& physical_plan) {
            using PlanType = std::decay_t<decltype(physical_plan)>;
            return std::is_same_v<PlanType, planner::PhysicalInsert> ||
                   std::is_same_v<PlanType, planner::PhysicalUpdate> ||
                   std::is_same_v<PlanType, planner::PhysicalDelete>;
        },
        plan);
}

}  // namespace

Database::Database()
    : runtime_(CreateInMemoryRuntime()) {}

Database::Database(std::shared_ptr<DatabaseRuntime> runtime)
    : runtime_(std::move(runtime)) {}

Database::~Database() {
    std::scoped_lock lock(session_mutex_);
    static_cast<void>(RollbackActiveTransaction());
}

Database::Database(Database&& other) noexcept
    : runtime_(std::move(other.runtime_)),
      in_memory_transaction_snapshot_(std::move(other.in_memory_transaction_snapshot_)),
      active_transaction_(std::move(other.active_transaction_)),
      explicit_transaction_active_(std::exchange(other.explicit_transaction_active_, false)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(session_mutex_, other.session_mutex_);
    static_cast<void>(RollbackActiveTransaction());
    runtime_ = std::move(other.runtime_);
    in_memory_transaction_snapshot_ = std::move(other.in_memory_transaction_snapshot_);
    active_transaction_ = std::move(other.active_transaction_);
    explicit_transaction_active_ = std::exchange(other.explicit_transaction_active_, false);
    return *this;
}

Result<Database> Database::Open(const std::filesystem::path& database_directory) {
    auto runtime = OpenPersistentRuntime(database_directory);
    if (!runtime.ok()) {
        return runtime.status();
    }
    return Database{std::move(runtime).value()};
}

Result<QueryResult> Database::Execute(std::string_view sql) {
    std::scoped_lock session_lock(session_mutex_);
    if (runtime_ == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "database session is not initialized");
    }

    const auto ast = parser::ParseSql(sql);
    if (!ast.ok()) {
        return ast.status();
    }

    if (std::holds_alternative<parser::AstBegin>(ast.value())) {
        return BeginTransaction();
    }
    if (std::holds_alternative<parser::AstCommit>(ast.value())) {
        return CommitTransaction();
    }
    if (std::holds_alternative<parser::AstRollback>(ast.value())) {
        return RollbackTransaction();
    }
    if (explicit_transaction_active_ && IsDdl(ast.value())) {
        return Status::Error(
            ErrorCode::kUnsupported,
            "CREATE TABLE and CREATE INDEX are not supported inside an explicit transaction");
    }

    const bool implicit_transaction = !explicit_transaction_active_;
    std::shared_ptr<transaction::Transaction> transaction = active_transaction_;
    if (implicit_transaction) {
        auto begun = runtime_->transaction_manager->Begin();
        if (!begun.ok()) {
            return begun.status();
        }
        transaction = std::move(begun).value();
        if (runtime_->table_storage != nullptr) {
            const Status storage_begin = runtime_->table_storage->BeginTransaction(transaction->id());
            if (!storage_begin.ok()) {
                static_cast<void>(runtime_->transaction_manager->Abort(transaction));
                return storage_begin;
            }
        }
    }

    const auto finish_on_error = [this, &transaction, implicit_transaction](const Status& error)
        -> Result<QueryResult> {
        if (implicit_transaction) {
            const Status finish_status = FinishImplicitTransaction(transaction, false, &error);
            if (!finish_status.ok()) {
                return finish_status;
            }
            return error;
        }
        return error;
    };

    const concurrency::LockMode catalog_mode = IsDdl(ast.value())
                                                     ? concurrency::LockMode::kExclusive
                                                     : concurrency::LockMode::kShared;
    const Status catalog_lock = runtime_->lock_manager->Acquire(
        *transaction,
        concurrency::LockResource::Catalog(),
        catalog_mode);
    if (!catalog_lock.ok()) {
        if (!implicit_transaction && catalog_lock.code() == ErrorCode::kDeadlock) {
            const Status rollback_status = RollbackActiveTransaction();
            if (!rollback_status.ok()) {
                return Status::Error(ErrorCode::kInternal, "deadlock victim rollback failed")
                    .WithContext("deadlock", catalog_lock.ToString())
                    .WithContext("rollback", rollback_status.ToString());
            }
            return catalog_lock;
        }
        return finish_on_error(catalog_lock);
    }

    const auto bound_statement = binder::BindStatement(ast.value(), *runtime_->catalog);
    if (!bound_statement.ok()) {
        return finish_on_error(bound_statement.status());
    }

    const auto physical_plan = planner::PlanStatement(bound_statement.value());
    if (!physical_plan.ok()) {
        return finish_on_error(physical_plan.status());
    }

    const auto lock_target = LockTarget(physical_plan.value());
    if (lock_target.has_value()) {
        const Status table_lock = runtime_->lock_manager->Acquire(
            *transaction,
            concurrency::LockResource::Table(lock_target->first),
            lock_target->second);
        if (!table_lock.ok()) {
            if (!implicit_transaction && table_lock.code() == ErrorCode::kDeadlock) {
                const Status rollback_status = RollbackActiveTransaction();
                if (!rollback_status.ok()) {
                    return Status::Error(ErrorCode::kInternal, "deadlock victim rollback failed")
                        .WithContext("deadlock", table_lock.ToString())
                        .WithContext("rollback", rollback_status.ToString());
                }
                return table_lock;
            }
            return finish_on_error(table_lock);
        }
    }

    execution::ExecutionContext context{
        .catalog = *runtime_->catalog,
        .table_storage = runtime_->table_storage.get(),
        .transaction_id = IsExplicitMutation(physical_plan.value())
                              ? std::optional<TransactionId>{transaction->id()}
                              : std::nullopt,
    };
    const auto execution_result = execution::ExecutePlan(physical_plan.value(), context);
    if (!implicit_transaction) {
        return execution_result;
    }

    const Status finish_status = FinishImplicitTransaction(
        transaction,
        execution_result.ok(),
        execution_result.ok() ? nullptr : &execution_result.status());
    if (!finish_status.ok()) {
        return finish_status;
    }
    return execution_result;
}

Result<QueryResult> Database::BeginTransaction() {
    if (explicit_transaction_active_) {
        return Status::Error(ErrorCode::kInvalidArgument, "a transaction is already active");
    }

    auto begun = runtime_->transaction_manager->Begin();
    if (!begun.ok()) {
        return begun.status();
    }
    std::shared_ptr<transaction::Transaction> transaction = std::move(begun).value();
    const Status catalog_lock = runtime_->lock_manager->Acquire(
        *transaction,
        concurrency::LockResource::Catalog(),
        concurrency::LockMode::kShared);
    if (!catalog_lock.ok()) {
        static_cast<void>(runtime_->transaction_manager->Abort(transaction));
        runtime_->lock_manager->ReleaseAll(transaction->id());
        return catalog_lock;
    }

    if (runtime_->table_storage != nullptr) {
        const Status storage_begin = runtime_->table_storage->BeginTransaction(transaction->id());
        if (!storage_begin.ok()) {
            static_cast<void>(runtime_->transaction_manager->Abort(transaction));
            runtime_->lock_manager->ReleaseAll(transaction->id());
            return storage_begin;
        }
    } else {
        in_memory_transaction_snapshot_ = std::make_unique<InMemoryCatalog>(*runtime_->catalog);
    }

    active_transaction_ = std::move(transaction);
    explicit_transaction_active_ = true;
    return TransactionControlResult(QueryResultKind::kBegin);
}

Result<QueryResult> Database::CommitTransaction() {
    if (!explicit_transaction_active_ || active_transaction_ == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "there is no active transaction to commit");
    }
    if (runtime_->table_storage != nullptr) {
        const Status status = runtime_->table_storage->CommitTransaction(active_transaction_->id());
        if (!status.ok()) {
            return status;
        }
    }
    const Status manager_status = runtime_->transaction_manager->Commit(active_transaction_);
    if (!manager_status.ok()) {
        return manager_status;
    }
    runtime_->lock_manager->ReleaseAll(active_transaction_->id());
    in_memory_transaction_snapshot_.reset();
    active_transaction_.reset();
    explicit_transaction_active_ = false;
    return TransactionControlResult(QueryResultKind::kCommit);
}

Result<QueryResult> Database::RollbackTransaction() {
    if (!explicit_transaction_active_) {
        return Status::Error(ErrorCode::kInvalidArgument, "there is no active transaction to roll back");
    }
    const Status status = RollbackActiveTransaction();
    if (!status.ok()) {
        return status;
    }
    return TransactionControlResult(QueryResultKind::kRollback);
}

Status Database::RollbackActiveTransaction() {
    if (!explicit_transaction_active_ || active_transaction_ == nullptr) {
        return Status::Ok();
    }

    Status rollback_status = Status::Ok();
    if (runtime_->table_storage != nullptr) {
        rollback_status = runtime_->table_storage->RollbackTransaction(active_transaction_->id());
    } else if (in_memory_transaction_snapshot_ != nullptr && runtime_->catalog != nullptr) {
        *runtime_->catalog = std::move(*in_memory_transaction_snapshot_);
    } else {
        rollback_status = Status::Error(ErrorCode::kInternal, "active transaction has no rollback state");
    }

    const Status manager_status = runtime_->transaction_manager->Abort(active_transaction_);
    runtime_->lock_manager->ReleaseAll(active_transaction_->id());
    in_memory_transaction_snapshot_.reset();
    active_transaction_.reset();
    explicit_transaction_active_ = false;
    if (!rollback_status.ok()) {
        return rollback_status;
    }
    return manager_status;
}

Status Database::FinishImplicitTransaction(
    const std::shared_ptr<transaction::Transaction>& transaction,
    bool succeeded,
    const Status* statement_error) {
    Status storage_status = Status::Ok();
    if (runtime_->table_storage != nullptr) {
        storage_status = succeeded
                             ? runtime_->table_storage->CommitTransaction(transaction->id())
                             : runtime_->table_storage->RollbackTransaction(transaction->id());
        if (succeeded && !storage_status.ok()) {
            static_cast<void>(runtime_->table_storage->RollbackTransaction(transaction->id()));
        }
    }

    const Status transaction_status = succeeded && storage_status.ok()
                                          ? runtime_->transaction_manager->Commit(transaction)
                                          : runtime_->transaction_manager->Abort(transaction);
    runtime_->lock_manager->ReleaseAll(transaction->id());

    if (!storage_status.ok()) {
        Status error = Status::Error(ErrorCode::kInternal, "implicit transaction storage finalization failed")
                           .WithContext("storage", storage_status.ToString());
        if (statement_error != nullptr) {
            error = error.WithContext("statement", statement_error->ToString());
        }
        return error;
    }
    return transaction_status;
}

}  // namespace kerndb
