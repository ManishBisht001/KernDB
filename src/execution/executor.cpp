#include "execution/executor.h"

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace kerndb::execution {
namespace {

[[nodiscard]] Result<QueryResult> ExecuteCreateTable(
    const planner::PhysicalCreateTable& plan,
    ExecutionContext& context) {
    const auto table_id = context.catalog.CreateTable(plan.table_name, plan.schema);
    if (!table_id.ok()) {
        return table_id.status();
    }
    if (context.table_storage != nullptr) {
        const auto table = context.catalog.FindTableById(table_id.value());
        if (!table.ok()) {
            static_cast<void>(context.catalog.RemoveTable(table_id.value()));
            return table.status();
        }
        const auto storage_status = context.table_storage->CreateTable(*table.value());
        if (!storage_status.ok()) {
            static_cast<void>(context.catalog.RemoveTable(table_id.value()));
            return storage_status.status();
        }
    }
    return QueryResult{
        .kind = QueryResultKind::kCreateTable,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteCreateIndex(
    const planner::PhysicalCreateIndex& plan,
    ExecutionContext& context) {
    const auto index_id = context.catalog.CreateIndex(
        plan.index_name,
        plan.table_id,
        plan.column_index);
    if (!index_id.ok()) {
        return index_id.status();
    }

    if (context.table_storage != nullptr) {
        const auto index = context.catalog.FindIndexById(index_id.value());
        if (!index.ok()) {
            static_cast<void>(context.catalog.RemoveIndex(index_id.value()));
            return index.status();
        }
        const auto root_page_id = context.table_storage->CreateIndex(*index.value());
        if (!root_page_id.ok()) {
            static_cast<void>(context.catalog.RemoveIndex(index_id.value()));
            return root_page_id.status();
        }
        const Status update_status = context.catalog.UpdateIndexRoot(
            index_id.value(),
            root_page_id.value());
        if (!update_status.ok()) {
            return update_status;
        }
    }

    return QueryResult{
        .kind = QueryResultKind::kCreateIndex,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteInsert(
    const planner::PhysicalInsert& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (context.table_storage != nullptr) {
        const auto record_id = context.table_storage->Insert(*table.value(), plan.values);
        if (!record_id.ok()) {
            return record_id.status();
        }
    } else {
        table.value()->tuples.push_back(plan.values);
    }
    return QueryResult{
        .kind = QueryResultKind::kInsert,
        .columns = {},
        .rows = {},
        .rows_affected = 1U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteSequentialScan(
    const planner::PhysicalSequentialScan& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }

    Schema projected_schema;
    projected_schema.reserve(plan.projection_indices.size());
    for (const std::size_t index : plan.projection_indices) {
        projected_schema.push_back(table.value()->schema[index]);
    }

    const auto source_rows = context.table_storage == nullptr
                                 ? Result<std::vector<Tuple>>{table.value()->tuples}
                                 : context.table_storage->Scan(*table.value());
    if (!source_rows.ok()) {
        return source_rows.status();
    }

    std::vector<Tuple> rows;
    for (const Tuple& tuple : source_rows.value()) {
        if (plan.predicate.has_value() &&
            !ValuesEqual(tuple[plan.predicate->column_index], plan.predicate->literal)) {
            continue;
        }

        Tuple projected_tuple;
        projected_tuple.reserve(plan.projection_indices.size());
        for (const std::size_t index : plan.projection_indices) {
            projected_tuple.push_back(tuple[index]);
        }
        rows.push_back(std::move(projected_tuple));
    }

    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(projected_schema),
        .rows = std::move(rows),
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteIndexScan(
    const planner::PhysicalIndexScan& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (context.table_storage == nullptr) {
        return ExecuteSequentialScan(
            planner::PhysicalSequentialScan{
                .table_id = plan.table_id,
                .projection_indices = plan.projection_indices,
                .predicate = plan.predicate,
            },
            context);
    }

    const auto key = std::get<std::int64_t>(plan.predicate.literal);
    const auto lookup = context.table_storage->LookupByIndex(
        *table.value(),
        plan.predicate.column_index,
        key);
    if (!lookup.ok()) {
        return lookup.status();
    }
    if (!lookup.value().index_available) {
        return ExecuteSequentialScan(
            planner::PhysicalSequentialScan{
                .table_id = plan.table_id,
                .projection_indices = plan.projection_indices,
                .predicate = plan.predicate,
            },
            context);
    }

    Schema projected_schema;
    projected_schema.reserve(plan.projection_indices.size());
    for (const std::size_t index : plan.projection_indices) {
        projected_schema.push_back(table.value()->schema[index]);
    }

    std::vector<Tuple> rows;
    if (lookup.value().tuple.has_value()) {
        const Tuple& tuple = lookup.value().tuple.value();
        if (!ValuesEqual(tuple[plan.predicate.column_index], plan.predicate.literal)) {
            return Status::Error(ErrorCode::kCorruption, "index returned a tuple with a mismatched key");
        }
        Tuple projected_tuple;
        projected_tuple.reserve(plan.projection_indices.size());
        for (const std::size_t index : plan.projection_indices) {
            projected_tuple.push_back(tuple[index]);
        }
        rows.push_back(std::move(projected_tuple));
    }

    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(projected_schema),
        .rows = std::move(rows),
        .rows_affected = 0U,
    };
}

}  // namespace

Result<QueryResult> ExecutePlan(
    const planner::PhysicalPlan& plan,
    ExecutionContext& context) {
    return std::visit(
        [&context](const auto& physical_plan) -> Result<QueryResult> {
            using PlanType = std::decay_t<decltype(physical_plan)>;
            if constexpr (std::is_same_v<PlanType, planner::PhysicalCreateTable>) {
                return ExecuteCreateTable(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalCreateIndex>) {
                return ExecuteCreateIndex(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalInsert>) {
                return ExecuteInsert(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalIndexScan>) {
                return ExecuteIndexScan(physical_plan, context);
            } else {
                return ExecuteSequentialScan(physical_plan, context);
            }
        },
        plan);
}

}  // namespace kerndb::execution
