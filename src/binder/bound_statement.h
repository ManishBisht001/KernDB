#pragma once

#include <cstddef>
#include <optional>
#include <cstdint>
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

struct BoundCreateIndex {
    std::string index_name;
    TableId table_id;
    std::size_t column_index;
};

struct BoundInsert {
    TableId table_id;
    std::vector<Value> values;
};

struct BoundPredicate {
    std::size_t column_index;
    Value literal;
    std::size_t relation_index{0U};
    enum class Comparison {
        kEqual,
        kNotEqual,
        kLess,
        kLessEqual,
        kGreater,
        kGreaterEqual,
    } comparison{Comparison::kEqual};
};

struct BoundColumnRef {
    std::size_t relation_index{0U};
    std::size_t column_index{0U};
    [[nodiscard]] bool operator==(const BoundColumnRef&) const = default;
};

enum class BoundAggregateFunction { kCount, kSum, kAvg, kMin, kMax };

struct BoundSelectItem {
    std::optional<BoundAggregateFunction> aggregate;
    std::optional<BoundColumnRef> column;
    bool count_star{false};
    ColumnDefinition output_column;
};

enum class BoundJoinKind { kInner, kLeft };

struct BoundJoin {
    BoundJoinKind kind{BoundJoinKind::kInner};
    TableId table_id;
    BoundColumnRef left_column;
    BoundColumnRef right_column;
};

struct BoundHaving {
    BoundAggregateFunction aggregate{BoundAggregateFunction::kCount};
    std::optional<BoundColumnRef> column;
    bool count_star{false};
    BoundPredicate::Comparison comparison{BoundPredicate::Comparison::kEqual};
    Value literal;
};

struct BoundOrderBy {
    BoundColumnRef column;
    bool ascending{true};
};

struct BoundSelect {
    TableId table_id;
    std::vector<std::size_t> projection_indices;
    std::optional<BoundPredicate> predicate;
    std::vector<BoundSelectItem> items;
    std::optional<BoundJoin> join;
    std::vector<BoundColumnRef> group_by;
    std::optional<BoundHaving> having;
    std::vector<BoundOrderBy> order_by;
    std::optional<std::uint64_t> limit;
    std::vector<std::string> table_names;
    std::vector<Schema> source_schemas;
    bool advanced{false};
    bool use_index{false};
};

struct BoundExplainSelect {
    BoundSelect select;
};

struct BoundUpdate {
    TableId table_id;
    std::size_t column_index;
    Value value;
    BoundPredicate predicate;
};

struct BoundDelete {
    TableId table_id;
    BoundPredicate predicate;
};

using BoundStatement = std::variant<
    BoundCreateTable,
    BoundCreateIndex,
    BoundInsert,
    BoundSelect,
    BoundExplainSelect,
    BoundUpdate,
    BoundDelete>;

}  // namespace kerndb::binder
