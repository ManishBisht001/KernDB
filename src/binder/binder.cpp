#include "binder/binder.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace kerndb::binder {
namespace {

[[nodiscard]] Status BindingError(const parser::SourceSpan& span, std::string message) {
    return Status::Error(ErrorCode::kBinding, std::move(message))
        .WithContext("offset", std::to_string(span.begin.offset))
        .WithContext("line", std::to_string(span.begin.line))
        .WithContext("column", std::to_string(span.begin.column));
}

[[nodiscard]] Result<const InMemoryTable*> ResolveTable(
    const parser::AstIdentifier& name,
    const InMemoryCatalog& catalog) {
    const auto table = catalog.FindTableByName(name.text);
    if (!table.ok()) {
        return BindingError(name.span, "table does not exist: " + NormalizeIdentifier(name.text));
    }
    return table.value();
}

[[nodiscard]] Result<std::size_t> ResolveColumn(
    const parser::AstIdentifier& name,
    const InMemoryTable& table) {
    const std::string normalized = NormalizeIdentifier(name.text);
    for (std::size_t index = 0U; index < table.schema.size(); ++index) {
        if (table.schema[index].name == normalized) {
            return index;
        }
    }
    return BindingError(name.span, "column does not exist: " + normalized);
}

[[nodiscard]] Result<BoundPredicate> BindPredicate(
    const parser::AstPredicate& predicate,
    const InMemoryTable& table) {
    const auto column_index = ResolveColumn(predicate.column_name, table);
    if (!column_index.ok()) {
        return column_index.status();
    }
    const ColumnDefinition& column = table.schema[column_index.value()];
    if (ValueType(predicate.literal.value) != column.type) {
        return Status::Error(
                   ErrorCode::kType,
                   "WHERE literal type does not match column type")
            .WithContext("column", column.name)
            .WithContext("expected", std::string(ColumnTypeName(column.type)))
            .WithContext("offset", std::to_string(predicate.literal.span.begin.offset))
            .WithContext("line", std::to_string(predicate.literal.span.begin.line))
            .WithContext(
                "column_number",
                std::to_string(predicate.literal.span.begin.column));
    }
    return BoundPredicate{
        .column_index = column_index.value(),
        .literal = predicate.literal.value,
    };
}

[[nodiscard]] BoundPredicate::Comparison BindComparison(
    parser::ComparisonOperator comparison) {
    switch (comparison) {
        case parser::ComparisonOperator::kEqual: return BoundPredicate::Comparison::kEqual;
        case parser::ComparisonOperator::kNotEqual: return BoundPredicate::Comparison::kNotEqual;
        case parser::ComparisonOperator::kLess: return BoundPredicate::Comparison::kLess;
        case parser::ComparisonOperator::kLessEqual: return BoundPredicate::Comparison::kLessEqual;
        case parser::ComparisonOperator::kGreater: return BoundPredicate::Comparison::kGreater;
        case parser::ComparisonOperator::kGreaterEqual: return BoundPredicate::Comparison::kGreaterEqual;
    }
    return BoundPredicate::Comparison::kEqual;
}

[[nodiscard]] Result<BoundColumnRef> ResolveColumnRef(
    const parser::AstColumnRef& reference,
    const std::vector<const InMemoryTable*>& tables) {
    const std::string column_name = NormalizeIdentifier(reference.name.text);
    if (reference.qualifier.has_value()) {
        const std::string qualifier = NormalizeIdentifier(reference.qualifier->text);
        for (std::size_t relation = 0U; relation < tables.size(); ++relation) {
            if (tables[relation]->name != qualifier) continue;
            const auto index = ResolveColumn(reference.name, *tables[relation]);
            if (!index.ok()) return index.status();
            return BoundColumnRef{.relation_index = relation, .column_index = index.value()};
        }
        return BindingError(reference.qualifier->span, "table is not present in FROM: " + qualifier);
    }

    std::optional<BoundColumnRef> resolved;
    for (std::size_t relation = 0U; relation < tables.size(); ++relation) {
        for (std::size_t column = 0U; column < tables[relation]->schema.size(); ++column) {
            if (tables[relation]->schema[column].name != column_name) continue;
            if (resolved.has_value()) {
                return BindingError(reference.name.span, "ambiguous column: " + column_name);
            }
            resolved = BoundColumnRef{.relation_index = relation, .column_index = column};
        }
    }
    if (!resolved.has_value()) {
        return BindingError(reference.name.span, "column does not exist: " + column_name);
    }
    return resolved.value();
}

[[nodiscard]] BoundAggregateFunction BindAggregate(parser::AstAggregateFunction function) {
    switch (function) {
        case parser::AstAggregateFunction::kCount: return BoundAggregateFunction::kCount;
        case parser::AstAggregateFunction::kSum: return BoundAggregateFunction::kSum;
        case parser::AstAggregateFunction::kAvg: return BoundAggregateFunction::kAvg;
        case parser::AstAggregateFunction::kMin: return BoundAggregateFunction::kMin;
        case parser::AstAggregateFunction::kMax: return BoundAggregateFunction::kMax;
    }
    return BoundAggregateFunction::kCount;
}

[[nodiscard]] std::string AggregateName(BoundAggregateFunction function) {
    switch (function) {
        case BoundAggregateFunction::kCount: return "count";
        case BoundAggregateFunction::kSum: return "sum";
        case BoundAggregateFunction::kAvg: return "avg";
        case BoundAggregateFunction::kMin: return "min";
        case BoundAggregateFunction::kMax: return "max";
    }
    return "aggregate";
}

[[nodiscard]] Result<ColumnType> BindAggregateInputType(
    BoundAggregateFunction aggregate,
    bool count_star,
    const std::optional<BoundColumnRef>& column,
    const std::vector<const InMemoryTable*>& tables,
    const parser::SourceSpan& span) {
    if (count_star) {
        if (aggregate != BoundAggregateFunction::kCount) {
            return BindingError(span, "only COUNT accepts '*'");
        }
        return ColumnType::kInt;
    }
    if (!column.has_value()) return BindingError(span, "aggregate requires a column argument");
    const ColumnType input_type = tables[column->relation_index]->schema[column->column_index].type;
    if ((aggregate == BoundAggregateFunction::kSum || aggregate == BoundAggregateFunction::kAvg) &&
        input_type != ColumnType::kInt) {
        return Status::Error(ErrorCode::kType, AggregateName(aggregate) + " requires an INT column")
            .WithContext("column", tables[column->relation_index]->schema[column->column_index].name);
    }
    if (aggregate == BoundAggregateFunction::kAvg) return ColumnType::kReal;
    if (aggregate == BoundAggregateFunction::kCount) return ColumnType::kInt;
    return input_type;
}

[[nodiscard]] std::string ColumnRefName(
    const BoundColumnRef& reference,
    const std::vector<const InMemoryTable*>& tables) {
    return tables[reference.relation_index]->schema[reference.column_index].name;
}

[[nodiscard]] Result<BoundSelect> BindSelectQuery(
    const parser::AstSelect& statement,
    const InMemoryCatalog& catalog) {
    if (statement.projections.empty()) {
        return Status::Error(ErrorCode::kBinding, "SELECT requires at least one projection");
    }
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) return table.status();
    std::vector<const InMemoryTable*> tables{table.value()};
    std::optional<BoundJoin> join;
    std::vector<std::string> table_names{table.value()->name};
    if (statement.join.has_value()) {
        const auto joined = ResolveTable(statement.join->table_name, catalog);
        if (!joined.ok()) return joined.status();
        if (joined.value()->id == table.value()->id) {
            return BindingError(statement.join->table_name.span, "self joins require table aliases, which are not supported");
        }
        tables.push_back(joined.value());
        table_names.push_back(joined.value()->name);
        const auto left_column = ResolveColumnRef(statement.join->left_column, tables);
        const auto right_column = ResolveColumnRef(statement.join->right_column, tables);
        if (!left_column.ok()) return left_column.status();
        if (!right_column.ok()) return right_column.status();
        BoundColumnRef join_left = left_column.value();
        BoundColumnRef join_right = right_column.value();
        if (join_left.relation_index == 1U && join_right.relation_index == 0U) {
            std::swap(join_left, join_right);
        }
        if (join_left.relation_index != 0U || join_right.relation_index != 1U) {
            return BindingError(statement.join->left_column.name.span, "JOIN condition must compare the left and right input columns");
        }
        const ColumnType left_type = tables[0]->schema[join_left.column_index].type;
        const ColumnType right_type = tables[1]->schema[join_right.column_index].type;
        if (left_type != right_type) {
            return Status::Error(ErrorCode::kType, "JOIN columns must have the same type")
                .WithContext("left", std::string(ColumnTypeName(left_type)))
                .WithContext("right", std::string(ColumnTypeName(right_type)));
        }
        join = BoundJoin{
            .kind = statement.join->kind == parser::AstJoinKind::kLeft
                        ? BoundJoinKind::kLeft
                        : BoundJoinKind::kInner,
            .table_id = joined.value()->id,
            .left_column = join_left,
            .right_column = join_right,
        };
    }

    std::optional<BoundPredicate> predicate;
    if (statement.predicate.has_value()) {
        const auto reference = ResolveColumnRef(statement.predicate->column, tables);
        if (!reference.ok()) return reference.status();
        const ColumnType column_type = tables[reference.value().relation_index]->schema[reference.value().column_index].type;
        if (ValueType(statement.predicate->literal.value) != column_type) {
            return Status::Error(ErrorCode::kType, "WHERE literal type does not match column type")
                .WithContext("column", ColumnRefName(reference.value(), tables))
                .WithContext("expected", std::string(ColumnTypeName(column_type)));
        }
        predicate = BoundPredicate{
            .column_index = reference.value().column_index,
            .literal = statement.predicate->literal.value,
            .relation_index = reference.value().relation_index,
            .comparison = BindComparison(statement.predicate->comparison),
        };
    }

    std::vector<BoundColumnRef> group_by;
    for (const parser::AstColumnRef& reference : statement.group_by) {
        const auto bound = ResolveColumnRef(reference, tables);
        if (!bound.ok()) return bound.status();
        group_by.push_back(bound.value());
    }

    std::vector<BoundSelectItem> items;
    items.reserve(statement.projections.size());
    std::vector<std::size_t> projection_indices;
    bool has_aggregate = false;
    for (const parser::AstSelectItem& item : statement.projections) {
        if (!item.aggregate.has_value()) {
            const auto column = ResolveColumnRef(item.column.value(), tables);
            if (!column.ok()) return column.status();
            const ColumnDefinition& source = tables[column.value().relation_index]->schema[column.value().column_index];
            items.push_back(BoundSelectItem{
                .aggregate = std::nullopt,
                .column = column.value(),
                .count_star = false,
                .output_column = ColumnDefinition{.name = source.name, .type = source.type},
            });
            if (column.value().relation_index == 0U) projection_indices.push_back(column.value().column_index);
            continue;
        }

        has_aggregate = true;
        const BoundAggregateFunction function = BindAggregate(item.aggregate.value());
        std::optional<BoundColumnRef> column;
        if (item.column.has_value()) {
            const auto resolved = ResolveColumnRef(item.column.value(), tables);
            if (!resolved.ok()) return resolved.status();
            column = resolved.value();
        }
        const auto output_type = BindAggregateInputType(function, item.count_star, column, tables, item.span);
        if (!output_type.ok()) return output_type.status();
        const std::string argument = item.count_star ? "*" : ColumnRefName(column.value(), tables);
        items.push_back(BoundSelectItem{
            .aggregate = function,
            .column = column,
            .count_star = item.count_star,
            .output_column = ColumnDefinition{
                .name = AggregateName(function) + "(" + argument + ")",
                .type = output_type.value(),
            },
        });
    }

    std::optional<BoundHaving> having;
    if (statement.having.has_value()) {
        has_aggregate = true;
        const BoundAggregateFunction function = BindAggregate(statement.having->aggregate);
        std::optional<BoundColumnRef> column;
        if (statement.having->column.has_value()) {
            const auto resolved = ResolveColumnRef(statement.having->column.value(), tables);
            if (!resolved.ok()) return resolved.status();
            column = resolved.value();
        }
        const auto aggregate_type = BindAggregateInputType(
            function, statement.having->count_star, column, tables, statement.having->literal.span);
        if (!aggregate_type.ok()) return aggregate_type.status();
        const ColumnType literal_type = ValueType(statement.having->literal.value);
        const bool numeric_comparison = aggregate_type.value() == ColumnType::kReal && literal_type == ColumnType::kInt;
        if (literal_type != aggregate_type.value() && !numeric_comparison) {
            return Status::Error(ErrorCode::kType, "HAVING literal type does not match aggregate result type")
                .WithContext("expected", std::string(ColumnTypeName(aggregate_type.value())));
        }
        having = BoundHaving{
            .aggregate = function,
            .column = column,
            .count_star = statement.having->count_star,
            .comparison = BindComparison(statement.having->comparison),
            .literal = statement.having->literal.value,
        };
    }

    const bool grouped = !group_by.empty() || has_aggregate;
    if (grouped) {
        for (const BoundSelectItem& item : items) {
            if (!item.aggregate.has_value() &&
                std::find(group_by.begin(), group_by.end(), item.column.value()) == group_by.end()) {
                return Status::Error(ErrorCode::kBinding, "selected columns must appear in GROUP BY or be aggregated")
                    .WithContext("column", item.output_column.name);
            }
        }
    }

    std::vector<BoundOrderBy> order_by;
    for (const parser::AstOrderBy& order : statement.order_by) {
        const auto column = ResolveColumnRef(order.column, tables);
        if (!column.ok()) return column.status();
        if (grouped && std::find(group_by.begin(), group_by.end(), column.value()) == group_by.end()) {
            return BindingError(order.column.name.span, "GROUP BY queries can order only by grouped columns");
        }
        order_by.push_back(BoundOrderBy{.column = column.value(), .ascending = order.ascending});
    }

    if (statement.limit.has_value() && statement.limit.value() < 0) {
        return Status::Error(ErrorCode::kInvalidArgument, "LIMIT must be a non-negative integer");
    }

    bool use_index = false;
    if (!join.has_value() && predicate.has_value() &&
        predicate->comparison == BoundPredicate::Comparison::kEqual &&
        predicate->relation_index == 0U && ValueType(predicate->literal) == ColumnType::kInt) {
        const auto index = catalog.FindIndexForTableColumn(table.value()->id, predicate->column_index);
        use_index = index.ok();
    }
    const bool advanced = join.has_value() || !group_by.empty() || having.has_value() ||
                          !order_by.empty() || statement.limit.has_value() || has_aggregate ||
                          (predicate.has_value() && predicate->comparison != BoundPredicate::Comparison::kEqual);
    return BoundSelect{
        .table_id = table.value()->id,
        .projection_indices = std::move(projection_indices),
        .predicate = std::move(predicate),
        .items = std::move(items),
        .join = std::move(join),
        .group_by = std::move(group_by),
        .having = std::move(having),
        .order_by = std::move(order_by),
        .limit = statement.limit.has_value()
                     ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(statement.limit.value())}
                     : std::nullopt,
        .table_names = std::move(table_names),
        .source_schemas = [&tables] {
            std::vector<Schema> schemas;
            schemas.reserve(tables.size());
            for (const InMemoryTable* source : tables) schemas.push_back(source->schema);
            return schemas;
        }(),
        .advanced = advanced,
        .use_index = use_index,
    };
}

[[nodiscard]] Result<BoundStatement> BindCreateTable(const parser::AstCreateTable& statement) {
    Schema schema;
    schema.reserve(statement.columns.size());
    std::set<std::string, std::less<>> names;
    for (const parser::AstColumnDefinition& column : statement.columns) {
        const std::string normalized = NormalizeIdentifier(column.name.text);
        if (!names.insert(normalized).second) {
            return BindingError(column.name.span, "duplicate column: " + normalized);
        }
        schema.push_back(ColumnDefinition{
            .name = normalized,
            .type = column.type,
        });
    }

    return BoundStatement{BoundCreateTable{
        .table_name = NormalizeIdentifier(statement.table_name.text),
        .schema = std::move(schema),
    }};
}

[[nodiscard]] Result<BoundStatement> BindCreateIndex(
    const parser::AstCreateIndex& statement,
    const InMemoryCatalog& catalog) {
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) {
        return table.status();
    }
    const auto column_index = ResolveColumn(statement.column_name, *table.value());
    if (!column_index.ok()) {
        return column_index.status();
    }
    if (table.value()->schema[column_index.value()].type != ColumnType::kInt) {
        return BindingError(statement.column_name.span, "Phase 4 indexes support INT columns only");
    }
    return BoundStatement{BoundCreateIndex{
        .index_name = NormalizeIdentifier(statement.index_name.text),
        .table_id = table.value()->id,
        .column_index = column_index.value(),
    }};
}

[[nodiscard]] Result<BoundStatement> BindInsert(
    const parser::AstInsert& statement,
    const InMemoryCatalog& catalog) {
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) {
        return table.status();
    }
    if (statement.values.size() != table.value()->schema.size()) {
        return BindingError(
            statement.table_name.span,
            "INSERT value count does not match table column count");
    }

    std::vector<Value> values;
    values.reserve(statement.values.size());
    for (std::size_t index = 0U; index < statement.values.size(); ++index) {
        const parser::AstLiteral& literal = statement.values[index];
        const ColumnDefinition& column = table.value()->schema[index];
        if (ValueType(literal.value) != column.type) {
            return Status::Error(
                       ErrorCode::kType,
                       "INSERT literal type does not match column type")
                .WithContext("column", column.name)
                .WithContext("expected", std::string(ColumnTypeName(column.type)))
                .WithContext("offset", std::to_string(literal.span.begin.offset))
                .WithContext("line", std::to_string(literal.span.begin.line))
                .WithContext("column_number", std::to_string(literal.span.begin.column));
        }
        values.push_back(literal.value);
    }

    return BoundStatement{BoundInsert{
        .table_id = table.value()->id,
        .values = std::move(values),
    }};
}

[[nodiscard]] Result<BoundStatement> BindSelect(
    const parser::AstSelect& statement,
    const InMemoryCatalog& catalog) {
    auto bound = BindSelectQuery(statement, catalog);
    if (!bound.ok()) return bound.status();
    return BoundStatement{std::move(bound).value()};
}

[[nodiscard]] Result<BoundStatement> BindExplainSelect(
    const parser::AstExplainSelect& statement,
    const InMemoryCatalog& catalog) {
    auto bound = BindSelectQuery(statement.select, catalog);
    if (!bound.ok()) return bound.status();
    return BoundStatement{BoundExplainSelect{.select = std::move(bound).value()}};
}

[[nodiscard]] Result<BoundStatement> BindUpdate(
    const parser::AstUpdate& statement,
    const InMemoryCatalog& catalog) {
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) {
        return table.status();
    }
    const auto column_index = ResolveColumn(statement.column_name, *table.value());
    if (!column_index.ok()) {
        return column_index.status();
    }
    const ColumnDefinition& column = table.value()->schema[column_index.value()];
    if (ValueType(statement.value.value) != column.type) {
        return Status::Error(
                   ErrorCode::kType,
                   "UPDATE literal type does not match column type")
            .WithContext("column", column.name)
            .WithContext("expected", std::string(ColumnTypeName(column.type)))
            .WithContext("offset", std::to_string(statement.value.span.begin.offset))
            .WithContext("line", std::to_string(statement.value.span.begin.line))
            .WithContext("column_number", std::to_string(statement.value.span.begin.column));
    }
    const auto predicate = BindPredicate(statement.predicate, *table.value());
    if (!predicate.ok()) {
        return predicate.status();
    }
    return BoundStatement{BoundUpdate{
        .table_id = table.value()->id,
        .column_index = column_index.value(),
        .value = statement.value.value,
        .predicate = predicate.value(),
    }};
}

[[nodiscard]] Result<BoundStatement> BindDelete(
    const parser::AstDelete& statement,
    const InMemoryCatalog& catalog) {
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) {
        return table.status();
    }
    const auto predicate = BindPredicate(statement.predicate, *table.value());
    if (!predicate.ok()) {
        return predicate.status();
    }
    return BoundStatement{BoundDelete{
        .table_id = table.value()->id,
        .predicate = predicate.value(),
    }};
}

}  // namespace

Result<BoundStatement> BindStatement(
    const parser::AstStatement& statement,
    const InMemoryCatalog& catalog) {
    return std::visit(
        [&catalog](const auto& concrete_statement) -> Result<BoundStatement> {
            using StatementType = std::decay_t<decltype(concrete_statement)>;
            if constexpr (std::is_same_v<StatementType, parser::AstCreateTable>) {
                return BindCreateTable(concrete_statement);
            } else if constexpr (std::is_same_v<StatementType, parser::AstCreateIndex>) {
                return BindCreateIndex(concrete_statement, catalog);
            } else if constexpr (std::is_same_v<StatementType, parser::AstInsert>) {
                return BindInsert(concrete_statement, catalog);
            } else if constexpr (std::is_same_v<StatementType, parser::AstSelect>) {
                return BindSelect(concrete_statement, catalog);
            } else if constexpr (std::is_same_v<StatementType, parser::AstExplainSelect>) {
                return BindExplainSelect(concrete_statement, catalog);
            } else if constexpr (std::is_same_v<StatementType, parser::AstUpdate>) {
                return BindUpdate(concrete_statement, catalog);
            } else if constexpr (std::is_same_v<StatementType, parser::AstDelete>) {
                return BindDelete(concrete_statement, catalog);
            } else {
                return Status::Error(
                    ErrorCode::kUnsupported,
                    "transaction control statements are handled by Database");
            }
        },
        statement);
}

}  // namespace kerndb::binder
