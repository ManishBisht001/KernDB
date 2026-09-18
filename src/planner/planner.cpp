#include "planner/planner.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace kerndb::planner {

LogicalPlan BuildLogicalPlan(const binder::BoundStatement& statement) {
    return std::visit(
        [](const auto& bound_statement) -> LogicalPlan {
            using StatementType = std::decay_t<decltype(bound_statement)>;
            if constexpr (std::is_same_v<StatementType, binder::BoundCreateTable>) {
                return LogicalCreateTable{
                    .table_name = bound_statement.table_name,
                    .schema = bound_statement.schema,
                };
            } else if constexpr (std::is_same_v<StatementType, binder::BoundInsert>) {
                return LogicalInsert{
                    .table_id = bound_statement.table_id,
                    .values = bound_statement.values,
                };
            } else {
                return LogicalSelect{
                    .table_id = bound_statement.table_id,
                    .projection_indices = bound_statement.projection_indices,
                    .predicate = bound_statement.predicate,
                };
            }
        },
        statement);
}

PhysicalPlan BuildPhysicalPlan(const LogicalPlan& plan) {
    return std::visit(
        [](const auto& logical_plan) -> PhysicalPlan {
            using PlanType = std::decay_t<decltype(logical_plan)>;
            if constexpr (std::is_same_v<PlanType, LogicalCreateTable>) {
                return PhysicalCreateTable{
                    .table_name = logical_plan.table_name,
                    .schema = logical_plan.schema,
                };
            } else if constexpr (std::is_same_v<PlanType, LogicalInsert>) {
                return PhysicalInsert{
                    .table_id = logical_plan.table_id,
                    .values = logical_plan.values,
                };
            } else {
                return PhysicalSequentialScan{
                    .table_id = logical_plan.table_id,
                    .projection_indices = logical_plan.projection_indices,
                    .predicate = logical_plan.predicate,
                };
            }
        },
        plan);
}

Result<PhysicalPlan> PlanStatement(const binder::BoundStatement& statement) {
    return BuildPhysicalPlan(BuildLogicalPlan(statement));
}

}  // namespace kerndb::planner
