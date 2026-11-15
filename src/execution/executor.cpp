#include "execution/executor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace kerndb::execution {
namespace {

using binder::BoundAggregateFunction;
using binder::BoundColumnRef;
using binder::BoundHaving;
using binder::BoundOrderBy;
using binder::BoundPredicate;
using binder::BoundSelectItem;

[[nodiscard]] Status ValidateInMemoryUniqueIndexes(
    const InMemoryCatalog& catalog,
    const InMemoryTable& table,
    const std::vector<Tuple>& tuples) {
    for (std::size_t column_index = 0U; column_index < table.schema.size(); ++column_index) {
        const auto index = catalog.FindIndexForTableColumn(table.id, column_index);
        if (!index.ok()) {
            if (index.status().code() == ErrorCode::kNotFound) {
                continue;
            }
            return index.status();
        }
        std::set<std::int64_t> keys;
        for (const Tuple& tuple : tuples) {
            const std::int64_t key = std::get<std::int64_t>(tuple[column_index]);
            if (!keys.insert(key).second) {
                return Status::Error(ErrorCode::kAlreadyExists, "duplicate key violates a unique index")
                    .WithContext("index", index.value()->name)
                    .WithContext("key", std::to_string(key));
            }
        }
    }
    return Status::Ok();
}

[[nodiscard]] Result<QueryResult> ExecuteCreateTable(
    const planner::PhysicalCreateTable& plan,
    ExecutionContext& context) {
    const auto table_id = context.catalog.CreateTable(plan.table_name, plan.schema);
    if (!table_id.ok()) {
        return table_id.status();
    }
    if (context.table_storage != nullptr) {
        const auto table = context.catalog.FindTableById(table_id.value());
        if (!table.ok()) {
            static_cast<void>(context.catalog.RemoveTable(table_id.value()));
            return table.status();
        }
        const auto storage_status = context.table_storage->CreateTable(*table.value());
        if (!storage_status.ok()) {
            static_cast<void>(context.catalog.RemoveTable(table_id.value()));
            return storage_status.status();
        }
    }
    return QueryResult{
        .kind = QueryResultKind::kCreateTable,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteCreateIndex(
    const planner::PhysicalCreateIndex& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (context.table_storage == nullptr) {
        std::set<std::int64_t> keys;
        for (const Tuple& tuple : table.value()->tuples) {
            const std::int64_t key = std::get<std::int64_t>(tuple[plan.column_index]);
            if (!keys.insert(key).second) {
                return Status::Error(ErrorCode::kAlreadyExists, "duplicate key violates a unique index")
                    .WithContext("index", plan.index_name)
                    .WithContext("key", std::to_string(key));
            }
        }
    }

    const auto index_id = context.catalog.CreateIndex(
        plan.index_name,
        plan.table_id,
        plan.column_index);
    if (!index_id.ok()) {
        return index_id.status();
    }

    if (context.table_storage != nullptr) {
        const auto index = context.catalog.FindIndexById(index_id.value());
        if (!index.ok()) {
            static_cast<void>(context.catalog.RemoveIndex(index_id.value()));
            return index.status();
        }
        const auto root_page_id = context.table_storage->CreateIndex(*index.value());
        if (!root_page_id.ok()) {
            static_cast<void>(context.catalog.RemoveIndex(index_id.value()));
            return root_page_id.status();
        }
        const Status update_status = context.catalog.UpdateIndexRoot(
            index_id.value(),
            root_page_id.value());
        if (!update_status.ok()) {
            return update_status;
        }
    }

    return QueryResult{
        .kind = QueryResultKind::kCreateIndex,
        .columns = {},
        .rows = {},
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteInsert(
    const planner::PhysicalInsert& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (context.table_storage != nullptr) {
        const auto record_id = context.table_storage->Insert(
            *table.value(),
            plan.values,
            context.transaction_id);
        if (!record_id.ok()) {
            return record_id.status();
        }
    } else {
        std::vector<Tuple> prospective_tuples = table.value()->tuples;
        prospective_tuples.push_back(plan.values);
        const Status uniqueness = ValidateInMemoryUniqueIndexes(
            context.catalog,
            *table.value(),
            prospective_tuples);
        if (!uniqueness.ok()) {
            return uniqueness;
        }
        table.value()->tuples.push_back(plan.values);
    }
    return QueryResult{
        .kind = QueryResultKind::kInsert,
        .columns = {},
        .rows = {},
        .rows_affected = 1U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteUpdate(
    const planner::PhysicalUpdate& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }

    std::uint64_t rows_affected = 0U;
    if (context.table_storage != nullptr) {
        const auto updated = context.table_storage->UpdateMatching(
            *table.value(),
            plan.column_index,
            plan.value,
            plan.predicate.column_index,
            plan.predicate.literal,
            context.transaction_id);
        if (!updated.ok()) {
            return updated.status();
        }
        rows_affected = updated.value();
    } else {
        std::vector<Tuple> prospective_tuples = table.value()->tuples;
        for (Tuple& tuple : prospective_tuples) {
            if (ValuesEqual(tuple[plan.predicate.column_index], plan.predicate.literal)) {
                tuple[plan.column_index] = plan.value;
                ++rows_affected;
            }
        }
        const Status uniqueness = ValidateInMemoryUniqueIndexes(
            context.catalog,
            *table.value(),
            prospective_tuples);
        if (!uniqueness.ok()) {
            return uniqueness;
        }
        table.value()->tuples = std::move(prospective_tuples);
    }

    return QueryResult{
        .kind = QueryResultKind::kUpdate,
        .columns = {},
        .rows = {},
        .rows_affected = rows_affected,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteDelete(
    const planner::PhysicalDelete& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }

    std::uint64_t rows_affected = 0U;
    if (context.table_storage != nullptr) {
        const auto deleted = context.table_storage->DeleteMatching(
            *table.value(),
            plan.predicate.column_index,
            plan.predicate.literal,
            context.transaction_id);
        if (!deleted.ok()) {
            return deleted.status();
        }
        rows_affected = deleted.value();
    } else {
        std::vector<Tuple> remaining_tuples;
        remaining_tuples.reserve(table.value()->tuples.size());
        for (const Tuple& tuple : table.value()->tuples) {
            if (ValuesEqual(tuple[plan.predicate.column_index], plan.predicate.literal)) {
                ++rows_affected;
            } else {
                remaining_tuples.push_back(tuple);
            }
        }
        table.value()->tuples = std::move(remaining_tuples);
    }

    return QueryResult{
        .kind = QueryResultKind::kDelete,
        .columns = {},
        .rows = {},
        .rows_affected = rows_affected,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteSequentialScan(
    const planner::PhysicalSequentialScan& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }

    Schema projected_schema;
    projected_schema.reserve(plan.projection_indices.size());
    for (const std::size_t index : plan.projection_indices) {
        projected_schema.push_back(table.value()->schema[index]);
    }

    const auto source_rows = context.table_storage == nullptr
                                 ? Result<std::vector<Tuple>>{table.value()->tuples}
                                 : context.table_storage->Scan(*table.value());
    if (!source_rows.ok()) {
        return source_rows.status();
    }

    std::vector<Tuple> rows;
    for (const Tuple& tuple : source_rows.value()) {
        if (plan.predicate.has_value() &&
            !ValuesEqual(tuple[plan.predicate->column_index], plan.predicate->literal)) {
            continue;
        }

        Tuple projected_tuple;
        projected_tuple.reserve(plan.projection_indices.size());
        for (const std::size_t index : plan.projection_indices) {
            projected_tuple.push_back(tuple[index]);
        }
        rows.push_back(std::move(projected_tuple));
    }

    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(projected_schema),
        .rows = std::move(rows),
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteIndexScan(
    const planner::PhysicalIndexScan& plan,
    ExecutionContext& context) {
    const auto table = context.catalog.FindTableById(plan.table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (context.table_storage == nullptr) {
        return ExecuteSequentialScan(
            planner::PhysicalSequentialScan{
                .table_id = plan.table_id,
                .projection_indices = plan.projection_indices,
                .predicate = plan.predicate,
            },
            context);
    }

    const auto key = std::get<std::int64_t>(plan.predicate.literal);
    const auto lookup = context.table_storage->LookupByIndex(
        *table.value(),
        plan.predicate.column_index,
        key);
    if (!lookup.ok()) {
        return lookup.status();
    }
    if (!lookup.value().index_available) {
        return ExecuteSequentialScan(
            planner::PhysicalSequentialScan{
                .table_id = plan.table_id,
                .projection_indices = plan.projection_indices,
                .predicate = plan.predicate,
            },
            context);
    }

    Schema projected_schema;
    projected_schema.reserve(plan.projection_indices.size());
    for (const std::size_t index : plan.projection_indices) {
        projected_schema.push_back(table.value()->schema[index]);
    }

    std::vector<Tuple> rows;
    if (lookup.value().tuple.has_value()) {
        const Tuple& tuple = lookup.value().tuple.value();
        if (!ValuesEqual(tuple[plan.predicate.column_index], plan.predicate.literal)) {
            return Status::Error(ErrorCode::kCorruption, "index returned a tuple with a mismatched key");
        }
        Tuple projected_tuple;
        projected_tuple.reserve(plan.projection_indices.size());
        for (const std::size_t index : plan.projection_indices) {
            projected_tuple.push_back(tuple[index]);
        }
        rows.push_back(std::move(projected_tuple));
    }

    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(projected_schema),
        .rows = std::move(rows),
        .rows_affected = 0U,
    };
}

[[nodiscard]] std::optional<int> CompareValues(const Value& left, const Value& right) {
    if (std::holds_alternative<std::monostate>(left) || std::holds_alternative<std::monostate>(right)) {
        return std::nullopt;
    }
    if (const auto* left_integer = std::get_if<std::int64_t>(&left); left_integer != nullptr) {
        if (const auto* right_integer = std::get_if<std::int64_t>(&right); right_integer != nullptr) {
            return *left_integer < *right_integer ? -1 : (*left_integer > *right_integer ? 1 : 0);
        }
        if (const auto* right_real = std::get_if<double>(&right); right_real != nullptr) {
            const double number = static_cast<double>(*left_integer);
            return number < *right_real ? -1 : (number > *right_real ? 1 : 0);
        }
    }
    if (const auto* left_real = std::get_if<double>(&left); left_real != nullptr) {
        if (const auto* right_real = std::get_if<double>(&right); right_real != nullptr) {
            return *left_real < *right_real ? -1 : (*left_real > *right_real ? 1 : 0);
        }
        if (const auto* right_integer = std::get_if<std::int64_t>(&right); right_integer != nullptr) {
            const double number = static_cast<double>(*right_integer);
            return *left_real < number ? -1 : (*left_real > number ? 1 : 0);
        }
    }
    if (const auto* left_text = std::get_if<std::string>(&left); left_text != nullptr) {
        if (const auto* right_text = std::get_if<std::string>(&right); right_text != nullptr) {
            return *left_text < *right_text ? -1 : (*left_text > *right_text ? 1 : 0);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool Satisfies(
    const Value& value,
    const Value& literal,
    binder::BoundPredicate::Comparison comparison) {
    const auto ordering = CompareValues(value, literal);
    if (!ordering.has_value()) return false;
    switch (comparison) {
        case binder::BoundPredicate::Comparison::kEqual: return ordering.value() == 0;
        case binder::BoundPredicate::Comparison::kNotEqual: return ordering.value() != 0;
        case binder::BoundPredicate::Comparison::kLess: return ordering.value() < 0;
        case binder::BoundPredicate::Comparison::kLessEqual: return ordering.value() <= 0;
        case binder::BoundPredicate::Comparison::kGreater: return ordering.value() > 0;
        case binder::BoundPredicate::Comparison::kGreaterEqual: return ordering.value() >= 0;
    }
    return false;
}

[[nodiscard]] std::size_t ColumnOffset(
    const binder::BoundColumnRef& column,
    const std::vector<Schema>& schemas) {
    std::size_t offset = column.column_index;
    for (std::size_t relation = 0U; relation < column.relation_index; ++relation) {
        offset += schemas[relation].size();
    }
    return offset;
}

[[nodiscard]] bool GroupValuesEqual(const Value& left, const Value& right) {
    if (std::holds_alternative<std::monostate>(left) || std::holds_alternative<std::monostate>(right)) {
        return std::holds_alternative<std::monostate>(left) && std::holds_alternative<std::monostate>(right);
    }
    return ValuesEqual(left, right);
}

[[nodiscard]] Result<std::vector<Tuple>> ScanSource(
    const InMemoryTable& table,
    ExecutionContext& context,
    bool use_index,
    const std::optional<BoundPredicate>& predicate,
    std::size_t relation_index) {
    if (use_index && relation_index == 0U && predicate.has_value() &&
        predicate->comparison == BoundPredicate::Comparison::kEqual &&
        std::holds_alternative<std::int64_t>(predicate->literal) && context.table_storage != nullptr) {
        const auto lookup = context.table_storage->LookupByIndex(
            table, predicate->column_index, std::get<std::int64_t>(predicate->literal));
        if (!lookup.ok()) return lookup.status();
        if (lookup.value().index_available) {
            std::vector<Tuple> rows;
            if (lookup.value().tuple.has_value()) {
                const Tuple& tuple = lookup.value().tuple.value();
                if (predicate->column_index >= tuple.size() ||
                    !ValuesEqual(tuple[predicate->column_index], predicate->literal)) {
                    return Status::Error(ErrorCode::kCorruption, "index returned a tuple with a mismatched key");
                }
                rows.push_back(tuple);
            }
            return rows;
        }
    }
    return context.table_storage == nullptr
               ? Result<std::vector<Tuple>>{table.tuples}
               : context.table_storage->Scan(table);
}

void PushDownPredicate(
    std::vector<Tuple>& rows,
    const std::optional<BoundPredicate>& predicate,
    std::size_t relation_index) {
    if (!predicate.has_value() || predicate->relation_index != relation_index) return;
    const BoundPredicate& condition = predicate.value();
    rows.erase(
        std::remove_if(rows.begin(), rows.end(), [&](const Tuple& row) {
            return !Satisfies(row[condition.column_index], condition.literal, condition.comparison);
        }),
        rows.end());
}

[[nodiscard]] Result<Value> ComputeAggregate(
    binder::BoundAggregateFunction aggregate,
    bool count_star,
    const std::optional<BoundColumnRef>& column,
    const std::vector<Tuple>& rows,
    const std::vector<Schema>& schemas) {
    if (aggregate == BoundAggregateFunction::kCount && count_star) {
        if (rows.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return Status::Error(ErrorCode::kResourceExhausted, "COUNT result exceeds INT range");
        }
        return Value{static_cast<std::int64_t>(rows.size())};
    }
    if (!column.has_value()) {
        return Status::Error(ErrorCode::kInternal, "bound aggregate has no input column");
    }
    const std::size_t offset = ColumnOffset(column.value(), schemas);
    std::int64_t count = 0;
    std::int64_t integer_sum = 0;
    long double real_sum = 0.0L;
    std::optional<Value> extremum;
    for (const Tuple& row : rows) {
        const Value& value = row[offset];
        if (std::holds_alternative<std::monostate>(value)) continue;
        if (aggregate == BoundAggregateFunction::kCount) {
            ++count;
            continue;
        }
        if (aggregate == BoundAggregateFunction::kSum) {
            const std::int64_t number = std::get<std::int64_t>(value);
            if ((number > 0 && integer_sum > std::numeric_limits<std::int64_t>::max() - number) ||
                (number < 0 && integer_sum < std::numeric_limits<std::int64_t>::min() - number)) {
                return Status::Error(ErrorCode::kResourceExhausted, "SUM result exceeds INT range");
            }
            integer_sum += number;
            ++count;
            continue;
        }
        if (aggregate == BoundAggregateFunction::kAvg) {
            real_sum += static_cast<long double>(std::get<std::int64_t>(value));
            ++count;
            continue;
        }
        if (!extremum.has_value()) {
            extremum = value;
            continue;
        }
        const auto comparison = CompareValues(value, extremum.value());
        if (!comparison.has_value()) {
            return Status::Error(ErrorCode::kType, "aggregate values have incompatible types");
        }
        if ((aggregate == BoundAggregateFunction::kMin && comparison.value() < 0) ||
            (aggregate == BoundAggregateFunction::kMax && comparison.value() > 0)) {
            extremum = value;
        }
    }

    if (aggregate == BoundAggregateFunction::kCount) return Value{count};
    if (aggregate == BoundAggregateFunction::kSum) {
        return count == 0 ? Value{std::monostate{}} : Value{integer_sum};
    }
    if (aggregate == BoundAggregateFunction::kAvg) {
        return count == 0 ? Value{std::monostate{}}
                          : Value{static_cast<double>(real_sum / static_cast<long double>(count))};
    }
    return extremum.has_value() ? extremum.value() : Value{std::monostate{}};
}

struct GroupBucket {
    Tuple key;
    std::vector<Tuple> rows;
};

[[nodiscard]] bool SameGroup(const Tuple& left, const Tuple& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (!GroupValuesEqual(left[index], right[index])) return false;
    }
    return true;
}

[[nodiscard]] Result<QueryResult> ExecuteAdvancedSelect(
    const planner::PhysicalAdvancedSelect& plan,
    ExecutionContext& context) {
    const binder::BoundSelect& select = plan.select;
    const auto left_table = context.catalog.FindTableById(select.table_id);
    if (!left_table.ok()) return left_table.status();
    auto left_rows = ScanSource(*left_table.value(), context, select.use_index, select.predicate, 0U);
    if (!left_rows.ok()) return left_rows.status();
    PushDownPredicate(left_rows.value(), select.predicate, 0U);

    std::vector<Tuple> input_rows;
    if (select.join.has_value()) {
        const auto right_table = context.catalog.FindTableById(select.join->table_id);
        if (!right_table.ok()) return right_table.status();
        auto right_rows = ScanSource(*right_table.value(), context, false, select.predicate, 1U);
        if (!right_rows.ok()) return right_rows.status();
        PushDownPredicate(right_rows.value(), select.predicate, 1U);
        const std::size_t left_key = ColumnOffset(select.join->left_column, select.source_schemas);
        for (const Tuple& left : left_rows.value()) {
            bool matched = false;
            for (const Tuple& right : right_rows.value()) {
                if (!ValuesEqual(left[left_key], right[select.join->right_column.column_index])) continue;
                Tuple joined = left;
                joined.insert(joined.end(), right.begin(), right.end());
                input_rows.push_back(std::move(joined));
                matched = true;
            }
            if (!matched && select.join->kind == binder::BoundJoinKind::kLeft) {
                Tuple joined = left;
                joined.insert(joined.end(), right_table.value()->schema.size(), Value{std::monostate{}});
                input_rows.push_back(std::move(joined));
            }
        }
    } else {
        input_rows = std::move(left_rows).value();
    }

    const bool requires_post_join_filter = select.join.has_value() &&
        select.join->kind == binder::BoundJoinKind::kLeft && select.predicate.has_value() &&
        select.predicate->relation_index == 1U;
    if (requires_post_join_filter) {
        const BoundPredicate& predicate = select.predicate.value();
        const binder::BoundColumnRef reference{
            .relation_index = predicate.relation_index,
            .column_index = predicate.column_index,
        };
        const std::size_t offset = ColumnOffset(reference, select.source_schemas);
        input_rows.erase(
            std::remove_if(input_rows.begin(), input_rows.end(), [&](const Tuple& row) {
                return !Satisfies(row[offset], predicate.literal, predicate.comparison);
            }),
            input_rows.end());
    }

    Schema output_schema;
    output_schema.reserve(select.items.size());
    for (const auto& item : select.items) output_schema.push_back(item.output_column);

    const bool aggregate_query = !select.group_by.empty() || select.having.has_value() ||
        std::any_of(select.items.begin(), select.items.end(), [](const auto& item) {
            return item.aggregate.has_value();
        });
    std::vector<Tuple> output_rows;
    if (!aggregate_query) {
        if (!select.order_by.empty()) {
            std::stable_sort(input_rows.begin(), input_rows.end(), [&](const Tuple& left, const Tuple& right) {
                for (const BoundOrderBy& order : select.order_by) {
                    const std::size_t offset = ColumnOffset(order.column, select.source_schemas);
                    const Value& left_value = left[offset];
                    const Value& right_value = right[offset];
                    const auto comparison = CompareValues(left_value, right_value);
                    if (!comparison.has_value()) {
                        const bool left_null = std::holds_alternative<std::monostate>(left_value);
                        const bool right_null = std::holds_alternative<std::monostate>(right_value);
                        if (left_null != right_null) return !left_null;  // NULLS LAST in either direction.
                    } else if (comparison.value() != 0) {
                        return order.ascending ? comparison.value() < 0 : comparison.value() > 0;
                    }
                }
                return false;
            });
        }
        output_rows.reserve(input_rows.size());
        for (const Tuple& input : input_rows) {
            Tuple projected;
            projected.reserve(select.items.size());
            for (const auto& item : select.items) {
                projected.push_back(input[ColumnOffset(item.column.value(), select.source_schemas)]);
            }
            output_rows.push_back(std::move(projected));
        }
    } else {
        std::vector<GroupBucket> groups;
        for (const Tuple& input : input_rows) {
            Tuple key;
            key.reserve(select.group_by.size());
            for (const BoundColumnRef& column : select.group_by) {
                key.push_back(input[ColumnOffset(column, select.source_schemas)]);
            }
            auto group = std::find_if(groups.begin(), groups.end(), [&](const GroupBucket& candidate) {
                return SameGroup(candidate.key, key);
            });
            if (group == groups.end()) {
                groups.push_back(GroupBucket{.key = std::move(key), .rows = {input}});
            } else {
                group->rows.push_back(input);
            }
        }
        if (select.group_by.empty() && groups.empty()) groups.push_back(GroupBucket{.key = {}, .rows = {}});

        struct GroupOutput {
            Tuple key;
            Tuple row;
        };
        std::vector<GroupOutput> grouped_output;
        for (const GroupBucket& group : groups) {
            if (select.having.has_value()) {
                const BoundHaving& having = select.having.value();
                const auto aggregate = ComputeAggregate(
                    having.aggregate, having.count_star, having.column, group.rows, select.source_schemas);
                if (!aggregate.ok()) return aggregate.status();
                if (!Satisfies(aggregate.value(), having.literal, having.comparison)) continue;
            }
            Tuple projected;
            projected.reserve(select.items.size());
            for (const BoundSelectItem& item : select.items) {
                if (item.aggregate.has_value()) {
                    const auto aggregate = ComputeAggregate(
                        item.aggregate.value(), item.count_star, item.column, group.rows, select.source_schemas);
                    if (!aggregate.ok()) return aggregate.status();
                    projected.push_back(aggregate.value());
                } else {
                    const auto group_column = std::find(select.group_by.begin(), select.group_by.end(), item.column.value());
                    if (group_column == select.group_by.end()) {
                        return Status::Error(ErrorCode::kInternal, "bound grouped projection is absent from GROUP BY");
                    }
                    projected.push_back(group.key[static_cast<std::size_t>(group_column - select.group_by.begin())]);
                }
            }
            grouped_output.push_back(GroupOutput{.key = group.key, .row = std::move(projected)});
        }
        if (!select.order_by.empty()) {
            std::stable_sort(grouped_output.begin(), grouped_output.end(), [&](const GroupOutput& left, const GroupOutput& right) {
                for (const BoundOrderBy& order : select.order_by) {
                    const auto key = std::find(select.group_by.begin(), select.group_by.end(), order.column);
                    if (key == select.group_by.end()) continue;
                    const std::size_t index = static_cast<std::size_t>(key - select.group_by.begin());
                    const auto comparison = CompareValues(left.key[index], right.key[index]);
                    if (!comparison.has_value()) {
                        const bool left_null = std::holds_alternative<std::monostate>(left.key[index]);
                        const bool right_null = std::holds_alternative<std::monostate>(right.key[index]);
                        if (left_null != right_null) return !left_null;
                    } else if (comparison.value() != 0) {
                        return order.ascending ? comparison.value() < 0 : comparison.value() > 0;
                    }
                }
                return false;
            });
        }
        output_rows.reserve(grouped_output.size());
        for (GroupOutput& group : grouped_output) output_rows.push_back(std::move(group.row));
    }

    if (select.limit.has_value() && output_rows.size() > select.limit.value()) {
        output_rows.resize(static_cast<std::size_t>(select.limit.value()));
    }
    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(output_schema),
        .rows = std::move(output_rows),
        .rows_affected = 0U,
    };
}

[[nodiscard]] Result<QueryResult> ExecuteExplain(const planner::PhysicalExplainSelect& plan) {
    Schema schema{{.name = "QUERY PLAN", .type = ColumnType::kText}};
    std::vector<Tuple> rows;
    std::size_t start = 0U;
    while (start < plan.plan_text.size()) {
        const std::size_t end = plan.plan_text.find('\n', start);
        rows.push_back(Tuple{Value{plan.plan_text.substr(start, end == std::string::npos ? end : end - start)}});
        if (end == std::string::npos) break;
        start = end + 1U;
    }
    return QueryResult{
        .kind = QueryResultKind::kSelect,
        .columns = std::move(schema),
        .rows = std::move(rows),
        .rows_affected = 0U,
    };
}

}  // namespace

Result<QueryResult> ExecutePlan(
    const planner::PhysicalPlan& plan,
    ExecutionContext& context) {
    return std::visit(
        [&context](const auto& physical_plan) -> Result<QueryResult> {
            using PlanType = std::decay_t<decltype(physical_plan)>;
            if constexpr (std::is_same_v<PlanType, planner::PhysicalCreateTable>) {
                return ExecuteCreateTable(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalCreateIndex>) {
                return ExecuteCreateIndex(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalInsert>) {
                return ExecuteInsert(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalIndexScan>) {
                return ExecuteIndexScan(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalAdvancedSelect>) {
                return ExecuteAdvancedSelect(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalExplainSelect>) {
                return ExecuteExplain(physical_plan);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalUpdate>) {
                return ExecuteUpdate(physical_plan, context);
            } else if constexpr (std::is_same_v<PlanType, planner::PhysicalDelete>) {
                return ExecuteDelete(physical_plan, context);
            } else {
                return ExecuteSequentialScan(physical_plan, context);
            }
        },
        plan);
}

}  // namespace kerndb::execution
