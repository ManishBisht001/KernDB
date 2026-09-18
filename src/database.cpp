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

Database::Database()
    : catalog_(std::make_unique<InMemoryCatalog>()) {}

Database::Database(
    std::unique_ptr<InMemoryCatalog> catalog,
    std::unique_ptr<storage::TableStorage> table_storage)
    : catalog_(std::move(catalog)),
      table_storage_(std::move(table_storage)) {}

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;

Database& Database::operator=(Database&&) noexcept = default;

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

}  // namespace kerndb
