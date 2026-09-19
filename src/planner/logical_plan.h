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

struct LogicalCreateIndex {
    std::string index_name;
    TableId table_id;
    std::size_t column_index;
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

using LogicalPlan = std::variant<LogicalCreateTable, LogicalCreateIndex, LogicalInsert, LogicalSelect>;

}  // namespace kerndb::planner
