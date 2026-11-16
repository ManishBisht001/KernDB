#include "planner/planner.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace kerndb::planner {
namespace {

[[nodiscard]] std::string ComparisonText(binder::BoundPredicate::Comparison comparison) {
    switch (comparison) {
        case binder::BoundPredicate::Comparison::kEqual: return "=";
        case binder::BoundPredicate::Comparison::kNotEqual: return "!=";
        case binder::BoundPredicate::Comparison::kLess: return "<";
        case binder::BoundPredicate::Comparison::kLessEqual: return "<=";
        case binder::BoundPredicate::Comparison::kGreater: return ">";
        case binder::BoundPredicate::Comparison::kGreaterEqual: return ">=";
    }
    return "?";
}

[[nodiscard]] std::string AggregateText(binder::BoundAggregateFunction function) {
    switch (function) {
        case binder::BoundAggregateFunction::kCount: return "COUNT";
        case binder::BoundAggregateFunction::kSum: return "SUM";
        case binder::BoundAggregateFunction::kAvg: return "AVG";
        case binder::BoundAggregateFunction::kMin: return "MIN";
        case binder::BoundAggregateFunction::kMax: return "MAX";
    }
    return "AGGREGATE";
}

[[nodiscard]] std::string ColumnText(const binder::BoundColumnRef& column, const binder::BoundSelect& select) {
    if (column.relation_index >= select.source_schemas.size() ||
        column.column_index >= select.source_schemas[column.relation_index].size()) {
        return "<invalid-column>";
    }
    return select.table_names[column.relation_index] + "." +
           select.source_schemas[column.relation_index][column.column_index].name;
}

[[nodiscard]] std::string ScanDescription(const binder::BoundSelect& select, bool physical) {
    if (physical && select.use_index && !select.join.has_value()) {
        return "Index Scan [" + select.table_names.front() + "]";
    }
    return "Sequential Scan [" + select.table_names.front() + "]";
}

[[nodiscard]] std::string PredicateLabel(const binder::BoundSelect& select) {
    if (!select.predicate.has_value()) return {};
    const auto& predicate = select.predicate.value();
    const binder::BoundColumnRef column{
        .relation_index = predicate.relation_index,
        .column_index = predicate.column_index,
    };
    return ColumnText(column, select) + " " + ComparisonText(predicate.comparison) + " " +
           ToString(predicate.literal);
}

[[nodiscard]] std::string Indent(std::string text) {
    std::string result = "  ";
    for (const char character : text) {
        result.push_back(character);
        if (character == '\n') result += "  ";
    }
    if (result.size() >= 2U && result.ends_with("  ")) result.resize(result.size() - 2U);
    return result;
}

[[nodiscard]] std::string SelectTree(const binder::BoundSelect& select, bool physical) {
    std::string tree;
    if (select.join.has_value()) {
        const std::string join_name = select.join->kind == binder::BoundJoinKind::kLeft
                                          ? "Nested Loop Join (LEFT)"
                                          : "Nested Loop Join (INNER)";
        const std::string condition = ColumnText(select.join->left_column, select) + " = " +
                                      ColumnText(select.join->right_column, select);
        std::string left_input = "Sequential Scan [" + select.table_names[0] + "]";
        std::string right_input = "Sequential Scan [" + select.table_names[1] + "]";
        if (physical && select.predicate.has_value()) {
            const std::string filter = "Filter [" + PredicateLabel(select) + "]\n";
            if (select.predicate->relation_index == 0U) left_input = filter + Indent(std::move(left_input));
            else right_input = filter + Indent(std::move(right_input));
        }
        tree = join_name + " [" + condition + "]\n" + Indent(std::move(left_input)) +
               "\n" + Indent(std::move(right_input));
    } else {
        tree = ScanDescription(select, physical);
    }

    const bool predicate_pushed_to_join_input = physical && select.join.has_value();
    const bool needs_residual = predicate_pushed_to_join_input &&
        select.join->kind == binder::BoundJoinKind::kLeft &&
        select.predicate.has_value() && select.predicate->relation_index == 1U;
    if (select.predicate.has_value() && (!predicate_pushed_to_join_input || needs_residual)) {
        tree = "Filter [" + PredicateLabel(select) + "]\n" + Indent(std::move(tree));
    }
    if (!select.group_by.empty() ||
        std::any_of(select.items.begin(), select.items.end(), [](const auto& item) {
            return item.aggregate.has_value();
        })) {
        std::string label = "Aggregate";
        if (!select.group_by.empty()) {
            label += " (GROUP BY ";
            for (std::size_t index = 0U; index < select.group_by.size(); ++index) {
                if (index != 0U) label += ", ";
                label += ColumnText(select.group_by[index], select);
            }
            label += ")";
        }
        tree = label + "\n" + Indent(std::move(tree));
    }
    if (select.having.has_value()) {
        const auto& having = select.having.value();
        tree = "Filter [HAVING " + AggregateText(having.aggregate) + " " +
               ComparisonText(having.comparison) + " " + ToString(having.literal) + "]\n" +
               Indent(std::move(tree));
    }
    if (!select.order_by.empty()) {
        std::ostringstream label;
        label << "Sort [";
        for (std::size_t index = 0U; index < select.order_by.size(); ++index) {
            if (index != 0U) label << ", ";
            label << ColumnText(select.order_by[index].column, select)
                  << (select.order_by[index].ascending ? " ASC" : " DESC");
        }
        label << "]\n" << Indent(std::move(tree));
        tree = label.str();
    }
    if (select.limit.has_value()) {
        tree = "Limit [" + std::to_string(select.limit.value()) + "]\n" + Indent(std::move(tree));
    }
    std::ostringstream projection;
    projection << "Projection [";
    for (std::size_t index = 0U; index < select.items.size(); ++index) {
        if (index != 0U) projection << ", ";
        projection << select.items[index].output_column.name;
    }
    projection << "]\n" << Indent(std::move(tree));
    return projection.str();
}

[[nodiscard]] std::string ExplainText(const binder::BoundSelect& select) {
    return "Logical Plan:\n" + SelectTree(select, false) + "\nPhysical Plan:\n" +
           SelectTree(select, true);
}

[[nodiscard]] binder::BoundStatement WithIndexChoice(
    const binder::BoundStatement& statement,
    bool persistent_storage_available) {
    return std::visit(
        [persistent_storage_available](const auto& bound) -> binder::BoundStatement {
            using Type = std::decay_t<decltype(bound)>;
            if constexpr (std::is_same_v<Type, binder::BoundSelect>) {
                auto copy = bound;
                copy.use_index = persistent_storage_available && bound.use_index;
                return copy;
            } else if constexpr (std::is_same_v<Type, binder::BoundExplainSelect>) {
                auto copy = bound;
                copy.select.use_index = persistent_storage_available && bound.select.use_index;
                return copy;
            } else {
                return bound;
            }
        },
        statement);
}

}  // namespace

LogicalPlan BuildLogicalPlan(const binder::BoundStatement& statement) {
    return std::visit(
        [](const auto& bound_statement) -> LogicalPlan {
            using StatementType = std::decay_t<decltype(bound_statement)>;
            if constexpr (std::is_same_v<StatementType, binder::BoundCreateTable>) {
                return LogicalCreateTable{.table_name = bound_statement.table_name, .schema = bound_statement.schema};
            } else if constexpr (std::is_same_v<StatementType, binder::BoundCreateIndex>) {
                return LogicalCreateIndex{.index_name = bound_statement.index_name, .table_id = bound_statement.table_id,
                                          .column_index = bound_statement.column_index};
            } else if constexpr (std::is_same_v<StatementType, binder::BoundInsert>) {
                return LogicalInsert{.table_id = bound_statement.table_id, .values = bound_statement.values};
            } else if constexpr (std::is_same_v<StatementType, binder::BoundSelect>) {
                if (bound_statement.advanced) return LogicalAdvancedSelect{.select = bound_statement};
                return LogicalSelect{.table_id = bound_statement.table_id,
                                     .projection_indices = bound_statement.projection_indices,
                                     .predicate = bound_statement.predicate,
                                     .use_index = bound_statement.use_index};
            } else if constexpr (std::is_same_v<StatementType, binder::BoundExplainSelect>) {
                return LogicalExplainSelect{.select = bound_statement.select};
            } else if constexpr (std::is_same_v<StatementType, binder::BoundUpdate>) {
                return LogicalUpdate{.table_id = bound_statement.table_id, .column_index = bound_statement.column_index,
                                     .value = bound_statement.value, .predicate = bound_statement.predicate};
            } else {
                return LogicalDelete{.table_id = bound_statement.table_id, .predicate = bound_statement.predicate};
            }
        },
        statement);
}

PhysicalPlan BuildPhysicalPlan(const LogicalPlan& plan) {
    return std::visit(
        [](const auto& logical_plan) -> PhysicalPlan {
            using PlanType = std::decay_t<decltype(logical_plan)>;
            if constexpr (std::is_same_v<PlanType, LogicalCreateTable>) {
                return PhysicalCreateTable{.table_name = logical_plan.table_name, .schema = logical_plan.schema};
            } else if constexpr (std::is_same_v<PlanType, LogicalCreateIndex>) {
                return PhysicalCreateIndex{.index_name = logical_plan.index_name, .table_id = logical_plan.table_id,
                                           .column_index = logical_plan.column_index};
            } else if constexpr (std::is_same_v<PlanType, LogicalInsert>) {
                return PhysicalInsert{.table_id = logical_plan.table_id, .values = logical_plan.values};
            } else if constexpr (std::is_same_v<PlanType, LogicalSelect>) {
                if (logical_plan.use_index && logical_plan.predicate.has_value()) {
                    return PhysicalIndexScan{.table_id = logical_plan.table_id,
                                             .projection_indices = logical_plan.projection_indices,
                                             .predicate = logical_plan.predicate.value()};
                }
                return PhysicalSequentialScan{.table_id = logical_plan.table_id,
                                              .projection_indices = logical_plan.projection_indices,
                                              .predicate = logical_plan.predicate};
            } else if constexpr (std::is_same_v<PlanType, LogicalAdvancedSelect>) {
                return PhysicalAdvancedSelect{.table_id = logical_plan.select.table_id,
                                               .joined_table_id = logical_plan.select.join.has_value()
                                                                      ? std::optional<TableId>{logical_plan.select.join->table_id}
                                                                      : std::nullopt,
                                               .select = logical_plan.select};
            } else if constexpr (std::is_same_v<PlanType, LogicalExplainSelect>) {
                return PhysicalExplainSelect{
                    .table_id = logical_plan.select.table_id,
                    .joined_table_id = logical_plan.select.join.has_value()
                                           ? std::optional<TableId>{logical_plan.select.join->table_id}
                                           : std::nullopt,
                    .plan_text = ExplainText(logical_plan.select),
                };
            } else if constexpr (std::is_same_v<PlanType, LogicalUpdate>) {
                return PhysicalUpdate{.table_id = logical_plan.table_id, .column_index = logical_plan.column_index,
                                      .value = logical_plan.value, .predicate = logical_plan.predicate};
            } else {
                return PhysicalDelete{.table_id = logical_plan.table_id, .predicate = logical_plan.predicate};
            }
        },
        plan);
}

Result<PhysicalPlan> PlanStatement(
    const binder::BoundStatement& statement,
    bool persistent_storage_available) {
    return BuildPhysicalPlan(BuildLogicalPlan(WithIndexChoice(statement, persistent_storage_available)));
}

}  // namespace kerndb::planner
