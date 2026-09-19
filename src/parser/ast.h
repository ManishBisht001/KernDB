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

struct AstSelect {
    std::vector<AstIdentifier> projections;
    AstIdentifier table_name;
    std::optional<AstPredicate> predicate;
};

using AstStatement = std::variant<AstCreateTable, AstCreateIndex, AstInsert, AstSelect>;

}  // namespace kerndb::parser
