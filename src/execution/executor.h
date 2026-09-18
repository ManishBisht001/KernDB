#pragma once

#include "execution_context.h"
#include "kerndb/result.h"
#include "kerndb/types.h"
#include "planner/physical_plan.h"

namespace kerndb::execution {

[[nodiscard]] Result<QueryResult> ExecutePlan(
    const planner::PhysicalPlan& plan,
    ExecutionContext& context);

}  // namespace kerndb::execution
