#include "kerndb/database.h"

#include <memory>
#include <string_view>
#include <utility>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "execution/execution_context.h"
#include "execution/executor.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "storage/persistent_storage.h"

namespace kerndb {
namespace {

[[nodiscard]] QueryResult TransactionControlResult(QueryResultKind kind) {
    return QueryResult{
        .kind = kind,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

}  // namespace

Database::Database()
    : catalog_(std::make_unique<InMemoryCatalog>()) {}

Database::Database(
    std::unique_ptr<InMemoryCatalog> catalog,
    std::unique_ptr<storage::TableStorage> table_storage)
    : catalog_(std::move(catalog)),
      table_storage_(std::move(table_storage)) {}

Database::~Database() {
    static_cast<void>(RollbackActiveTransaction());
}

Database::Database(Database&& other) noexcept
    : catalog_(std::move(other.catalog_)),
      table_storage_(std::move(other.table_storage_)),
      in_memory_transaction_snapshot_(std::move(other.in_memory_transaction_snapshot_)),
      explicit_transaction_active_(std::exchange(other.explicit_transaction_active_, false)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    static_cast<void>(RollbackActiveTransaction());
    table_storage_ = std::move(other.table_storage_);
    catalog_ = std::move(other.catalog_);
    in_memory_transaction_snapshot_ = std::move(other.in_memory_transaction_snapshot_);
    explicit_transaction_active_ = std::exchange(other.explicit_transaction_active_, false);
    return *this;
}

Result<Database> Database::Open(const std::filesystem::path& database_directory) {
    auto catalog = std::make_unique<InMemoryCatalog>();
    auto persistent_storage = storage::OpenPersistentStorage(database_directory, *catalog);
    if (!persistent_storage.ok()) {
        return persistent_storage.status();
    }
    std::unique_ptr<storage::TableStorage> table_storage = std::move(persistent_storage).value();
    return Database{std::move(catalog), std::move(table_storage)};
}

Result<QueryResult> Database::Execute(std::string_view sql) {
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
    if (explicit_transaction_active_ &&
        (std::holds_alternative<parser::AstCreateTable>(ast.value()) ||
         std::holds_alternative<parser::AstCreateIndex>(ast.value()))) {
        return Status::Error(
            ErrorCode::kUnsupported,
            "CREATE TABLE and CREATE INDEX are not supported inside an explicit transaction");
    }

    const auto bound_statement = binder::BindStatement(ast.value(), *catalog_);
    if (!bound_statement.ok()) {
        return bound_statement.status();
    }

    const auto physical_plan = planner::PlanStatement(bound_statement.value());
    if (!physical_plan.ok()) {
        return physical_plan.status();
    }

    execution::ExecutionContext context{
        .catalog = *catalog_,
        .table_storage = table_storage_.get(),
    };
    return execution::ExecutePlan(physical_plan.value(), context);
}

Result<QueryResult> Database::BeginTransaction() {
    if (explicit_transaction_active_) {
        return Status::Error(ErrorCode::kInvalidArgument, "a transaction is already active");
    }
    if (table_storage_ != nullptr) {
        const Status status = table_storage_->BeginTransaction();
        if (!status.ok()) {
            return status;
        }
    } else {
        in_memory_transaction_snapshot_ = std::make_unique<InMemoryCatalog>(*catalog_);
    }
    explicit_transaction_active_ = true;
    return TransactionControlResult(QueryResultKind::kBegin);
}

Result<QueryResult> Database::CommitTransaction() {
    if (!explicit_transaction_active_) {
        return Status::Error(ErrorCode::kInvalidArgument, "there is no active transaction to commit");
    }
    if (table_storage_ != nullptr) {
        const Status status = table_storage_->CommitTransaction();
        if (!status.ok()) {
            return status;
        }
    }
    in_memory_transaction_snapshot_.reset();
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
    if (!explicit_transaction_active_) {
        return Status::Ok();
    }

    Status status = Status::Ok();
    if (table_storage_ != nullptr) {
        status = table_storage_->RollbackTransaction();
    } else if (in_memory_transaction_snapshot_ != nullptr && catalog_ != nullptr) {
        *catalog_ = std::move(*in_memory_transaction_snapshot_);
    } else {
        status = Status::Error(ErrorCode::kInternal, "active transaction has no rollback state");
    }

    explicit_transaction_active_ = false;
    in_memory_transaction_snapshot_.reset();
    return status;
}

}  // namespace kerndb
