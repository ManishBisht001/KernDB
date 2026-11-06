#include "binder/binder.h"

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
    const auto table = ResolveTable(statement.table_name, catalog);
    if (!table.ok()) {
        return table.status();
    }

    std::vector<std::size_t> projections;
    projections.reserve(statement.projections.size());
    for (const parser::AstIdentifier& projection : statement.projections) {
        const auto index = ResolveColumn(projection, *table.value());
        if (!index.ok()) {
            return index.status();
        }
        projections.push_back(index.value());
    }

    std::optional<BoundPredicate> predicate;
    if (statement.predicate.has_value()) {
        const auto bound_predicate = BindPredicate(statement.predicate.value(), *table.value());
        if (!bound_predicate.ok()) {
            return bound_predicate.status();
        }
        predicate = bound_predicate.value();
    }

    return BoundStatement{BoundSelect{
        .table_id = table.value()->id,
        .projection_indices = std::move(projections),
        .predicate = std::move(predicate),
    }};
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
