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

struct PhysicalCreateIndex {
    std::string index_name;
    TableId table_id;
    std::size_t column_index;
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

struct PhysicalIndexScan {
    TableId table_id;
    std::vector<std::size_t> projection_indices;
    binder::BoundPredicate predicate;
};

struct PhysicalAdvancedSelect {
    TableId table_id;
    std::optional<TableId> joined_table_id;
    binder::BoundSelect select;
};

struct PhysicalExplainSelect {
    TableId table_id;
    std::optional<TableId> joined_table_id;
    std::string plan_text;
};

struct PhysicalUpdate {
    TableId table_id;
    std::size_t column_index;
    Value value;
    binder::BoundPredicate predicate;
};

struct PhysicalDelete {
    TableId table_id;
    binder::BoundPredicate predicate;
};

using PhysicalPlan = std::variant<
    PhysicalCreateTable,
    PhysicalCreateIndex,
    PhysicalInsert,
    PhysicalSequentialScan,
    PhysicalIndexScan,
    PhysicalAdvancedSelect,
    PhysicalExplainSelect,
    PhysicalUpdate,
    PhysicalDelete>;

}  // namespace kerndb::planner
