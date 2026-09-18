#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "binder/bound_statement.h"

namespace kerndb::planner {

struct PhysicalCreateTable {
    std::string table_name;
    Schema schema;
};

struct PhysicalInsert {
    TableId table_id;
    std::vector<Value> values;
};

struct PhysicalSequentialScan {
    TableId table_id;
    std::vector<std::size_t> projection_indices;
    std::optional<binder::BoundPredicate> predicate;
};

using PhysicalPlan = std::variant<PhysicalCreateTable, PhysicalInsert, PhysicalSequentialScan>;

}  // namespace kerndb::planner
