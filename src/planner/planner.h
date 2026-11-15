#pragma once

#include "binder/bound_statement.h"
#include "kerndb/result.h"
#include "logical_plan.h"
#include "physical_plan.h"

namespace kerndb::planner {

[[nodiscard]] LogicalPlan BuildLogicalPlan(const binder::BoundStatement& statement);
[[nodiscard]] PhysicalPlan BuildPhysicalPlan(const LogicalPlan& plan);
[[nodiscard]] Result<PhysicalPlan> PlanStatement(
    const binder::BoundStatement& statement,
    bool persistent_storage_available = false);

}  // namespace kerndb::planner
