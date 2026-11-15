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
    bool use_index{false};
};

struct LogicalAdvancedSelect {
    binder::BoundSelect select;
};

struct LogicalExplainSelect {
    binder::BoundSelect select;
};

struct LogicalUpdate {
    TableId table_id;
    std::size_t column_index;
    Value value;
    binder::BoundPredicate predicate;
};

struct LogicalDelete {
    TableId table_id;
    binder::BoundPredicate predicate;
};

using LogicalPlan = std::variant<
    LogicalCreateTable,
    LogicalCreateIndex,
    LogicalInsert,
    LogicalSelect,
    LogicalAdvancedSelect,
    LogicalExplainSelect,
    LogicalUpdate,
    LogicalDelete>;

}  // namespace kerndb::planner
