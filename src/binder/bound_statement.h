#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/types.h"

namespace kerndb::binder {

struct BoundCreateTable {
    std::string table_name;
    Schema schema;
};

struct BoundInsert {
    TableId table_id;
    std::vector<Value> values;
};

struct BoundPredicate {
    std::size_t column_index;
    Value literal;
};

struct BoundSelect {
    TableId table_id;
    std::vector<std::size_t> projection_indices;
    std::optional<BoundPredicate> predicate;
};

using BoundStatement = std::variant<BoundCreateTable, BoundInsert, BoundSelect>;

}  // namespace kerndb::binder
