#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kerndb/types.h"
#include "token.h"

namespace kerndb::parser {

struct AstIdentifier {
    std::string text;
    SourceSpan span;
};

struct AstLiteral {
    Value value;
    SourceSpan span;
};

struct AstColumnRef {
    std::optional<AstIdentifier> qualifier;
    AstIdentifier name;
};

enum class ComparisonOperator {
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};

enum class AstAggregateFunction {
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
};

struct AstColumnDefinition {
    AstIdentifier name;
    ColumnType type;
    SourceSpan type_span;
};

struct AstCreateTable {
    AstIdentifier table_name;
    std::vector<AstColumnDefinition> columns;
};

struct AstCreateIndex {
    AstIdentifier index_name;
    AstIdentifier table_name;
    AstIdentifier column_name;
};

struct AstInsert {
    AstIdentifier table_name;
    std::vector<AstLiteral> values;
};

struct AstPredicate {
    AstIdentifier column_name;
    AstLiteral literal;
};

struct AstSelectPredicate {
    AstColumnRef column;
    AstLiteral literal;
    ComparisonOperator comparison{ComparisonOperator::kEqual};
};

struct AstSelectItem {
    std::optional<AstAggregateFunction> aggregate;
    std::optional<AstColumnRef> column;
    bool count_star{false};
    SourceSpan span;
};

enum class AstJoinKind {
    kInner,
    kLeft,
};

struct AstJoin {
    AstJoinKind kind{AstJoinKind::kInner};
    AstIdentifier table_name;
    AstColumnRef left_column;
    AstColumnRef right_column;
};

struct AstGroupHaving {
    AstAggregateFunction aggregate{AstAggregateFunction::kCount};
    std::optional<AstColumnRef> column;
    bool count_star{false};
    ComparisonOperator comparison{ComparisonOperator::kEqual};
    AstLiteral literal;
};

struct AstOrderBy {
    AstColumnRef column;
    bool ascending{true};
};

struct AstSelect {
    std::vector<AstSelectItem> projections;
    AstIdentifier table_name;
    std::optional<AstSelectPredicate> predicate;
    std::optional<AstJoin> join;
    std::vector<AstColumnRef> group_by;
    std::optional<AstGroupHaving> having;
    std::vector<AstOrderBy> order_by;
    std::optional<std::int64_t> limit;
};

struct AstExplainSelect {
    AstSelect select;
};

struct AstUpdate {
    AstIdentifier table_name;
    AstIdentifier column_name;
    AstLiteral value;
    AstPredicate predicate;
};

struct AstDelete {
    AstIdentifier table_name;
    AstPredicate predicate;
};

struct AstBegin {};

struct AstCommit {};

struct AstRollback {};

using AstStatement = std::variant<
    AstCreateTable,
    AstCreateIndex,
    AstInsert,
    AstSelect,
    AstExplainSelect,
    AstUpdate,
    AstDelete,
    AstBegin,
    AstCommit,
    AstRollback>;

}  // namespace kerndb::parser
