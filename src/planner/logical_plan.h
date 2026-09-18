#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "binder/bound_statement.h"

namespace kerndb::planner {

struct LogicalCreateTable {
    std::string table_name;
    Schema schema;
};

struct LogicalInsert {
    TableId table_id;
    std::vector<Value> values;
};

struct LogicalSelect {
    TableId table_id;
    std::vector<std::size_t> projection_indices;
    std::optional<binder::BoundPredicate> predicate;
};

using LogicalPlan = std::variant<LogicalCreateTable, LogicalInsert, LogicalSelect>;

}  // namespace kerndb::planner
