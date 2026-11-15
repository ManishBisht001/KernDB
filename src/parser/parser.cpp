#include "parser.h"

#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "lexer.h"

namespace kerndb::parser {
namespace {

[[nodiscard]] Status SyntaxError(const SourceSpan& span, std::string message) {
    return Status::Error(ErrorCode::kSyntax, std::move(message))
        .WithContext("offset", std::to_string(span.begin.offset))
        .WithContext("line", std::to_string(span.begin.line))
        .WithContext("column", std::to_string(span.begin.column));
}

class StatementParser {
public:
    explicit StatementParser(const std::vector<Token>& tokens)
        : tokens_(tokens) {}

    [[nodiscard]] Result<AstStatement> Parse() {
        Result<AstStatement> statement = ParseStatement();
        if (!statement.ok()) {
            return statement.status();
        }

        if (Match(TokenKind::kSemicolon) && Current().kind != TokenKind::kEnd) {
            return SyntaxError(Current().span, "expected end of input after semicolon");
        }
        if (Current().kind != TokenKind::kEnd) {
            return SyntaxError(Current().span, "expected end of input");
        }
        return statement;
    }

private:
    [[nodiscard]] const Token& Current() const {
        return tokens_[position_];
    }

    [[nodiscard]] bool Match(TokenKind kind) {
        if (Current().kind != kind) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] Result<Token> Expect(TokenKind kind, std::string expectation) {
        if (Current().kind != kind) {
            return SyntaxError(
                Current().span,
                "expected " + expectation + ", found " +
                    std::string(TokenKindName(Current().kind)));
        }
        return tokens_[position_++];
    }

    [[nodiscard]] Result<AstIdentifier> ParseIdentifier() {
        const auto token = Expect(TokenKind::kIdentifier, "identifier");
        if (!token.ok()) {
            return token.status();
        }
        return AstIdentifier{
            .text = token.value().lexeme,
            .span = token.value().span,
        };
    }

    [[nodiscard]] Result<AstLiteral> ParseLiteral() {
        bool negative = false;
        SourceSpan span = Current().span;
        if (Match(TokenKind::kMinus)) {
            negative = true;
        }

        if (Current().kind == TokenKind::kStringLiteral) {
            if (negative) {
                return SyntaxError(Current().span, "a minus sign can only prefix an integer literal");
            }
            const Token token = tokens_[position_++];
            return AstLiteral{
                .value = Value{token.lexeme},
                .span = token.span,
            };
        }

        const auto token = Expect(TokenKind::kIntegerLiteral, "literal");
        if (!token.ok()) {
            return token.status();
        }
        if (!negative) {
            span = token.value().span;
        } else {
            span.end = token.value().span.end;
        }

        std::string text = negative ? "-" : "";
        text += token.value().lexeme;
        std::int64_t value = 0;
        const auto [end, error] = std::from_chars(
            text.data(),
            text.data() + text.size(),
            value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return SyntaxError(span, "integer literal is outside the INT range");
        }
        return AstLiteral{
            .value = Value{value},
            .span = span,
        };
    }

    [[nodiscard]] Result<AstPredicate> ParsePredicate() {
        const auto column_name = ParseIdentifier();
        if (!column_name.ok()) {
            return column_name.status();
        }
        const auto equal = Expect(TokenKind::kEqual, "'=' in predicate");
        if (!equal.ok()) return equal.status();
        const auto literal = ParseLiteral();
        if (!literal.ok()) {
            return literal.status();
        }
        return AstPredicate{
            .column_name = column_name.value(),
            .literal = literal.value(),
        };
    }

    [[nodiscard]] Result<ComparisonOperator> ParseComparisonOperator() {
        const TokenKind kind = Current().kind;
        switch (kind) {
            case TokenKind::kEqual: ++position_; return ComparisonOperator::kEqual;
            case TokenKind::kNotEqual: ++position_; return ComparisonOperator::kNotEqual;
            case TokenKind::kLess: ++position_; return ComparisonOperator::kLess;
            case TokenKind::kLessEqual: ++position_; return ComparisonOperator::kLessEqual;
            case TokenKind::kGreater: ++position_; return ComparisonOperator::kGreater;
            case TokenKind::kGreaterEqual: ++position_; return ComparisonOperator::kGreaterEqual;
            default:
                return SyntaxError(Current().span, "expected a comparison operator");
        }
    }

    [[nodiscard]] Result<AstColumnRef> ParseColumnRef() {
        const auto first = ParseIdentifier();
        if (!first.ok()) return first.status();
        if (!Match(TokenKind::kDot)) {
            return AstColumnRef{.qualifier = std::nullopt, .name = first.value()};
        }
        const auto column = ParseIdentifier();
        if (!column.ok()) return column.status();
        return AstColumnRef{.qualifier = first.value(), .name = column.value()};
    }

    [[nodiscard]] std::optional<AstAggregateFunction> CurrentAggregate() const {
        switch (Current().kind) {
            case TokenKind::kCount: return AstAggregateFunction::kCount;
            case TokenKind::kSum: return AstAggregateFunction::kSum;
            case TokenKind::kAvg: return AstAggregateFunction::kAvg;
            case TokenKind::kMin: return AstAggregateFunction::kMin;
            case TokenKind::kMax: return AstAggregateFunction::kMax;
            default: return std::nullopt;
        }
    }

    [[nodiscard]] Result<AstSelectItem> ParseSelectItem() {
        const SourceLocation start = Current().span.begin;
        const auto aggregate = CurrentAggregate();
        if (!aggregate.has_value()) {
            const auto column = ParseColumnRef();
            if (!column.ok()) return column.status();
            return AstSelectItem{
                .aggregate = std::nullopt,
                .column = column.value(),
                .count_star = false,
                .span = SourceSpan{start, column.value().name.span.end},
            };
        }

        ++position_;
        const auto left = Expect(TokenKind::kLeftParen, "'(' after aggregate function");
        if (!left.ok()) return left.status();
        bool count_star = false;
        std::optional<AstColumnRef> column;
        if (Match(TokenKind::kStar)) {
            count_star = true;
            if (aggregate.value() != AstAggregateFunction::kCount) {
                return SyntaxError(tokens_[position_ - 1U].span, "only COUNT accepts '*'");
            }
        } else {
            const auto parsed_column = ParseColumnRef();
            if (!parsed_column.ok()) return parsed_column.status();
            column = parsed_column.value();
        }
        const auto right = Expect(TokenKind::kRightParen, "')' after aggregate argument");
        if (!right.ok()) return right.status();
        return AstSelectItem{
            .aggregate = aggregate,
            .column = std::move(column),
            .count_star = count_star,
            .span = SourceSpan{start, right.value().span.end},
        };
    }

    [[nodiscard]] Result<AstGroupHaving> ParseHaving() {
        const auto aggregate = CurrentAggregate();
        if (!aggregate.has_value()) {
            return SyntaxError(Current().span, "HAVING requires an aggregate function");
        }
        ++position_;
        const auto left = Expect(TokenKind::kLeftParen, "'(' after HAVING aggregate");
        if (!left.ok()) return left.status();
        bool count_star = false;
        std::optional<AstColumnRef> column;
        if (Match(TokenKind::kStar)) {
            count_star = true;
            if (aggregate.value() != AstAggregateFunction::kCount) {
                return SyntaxError(tokens_[position_ - 1U].span, "only COUNT accepts '*'");
            }
        } else {
            const auto parsed_column = ParseColumnRef();
            if (!parsed_column.ok()) return parsed_column.status();
            column = parsed_column.value();
        }
        const auto right = Expect(TokenKind::kRightParen, "')' after HAVING aggregate");
        if (!right.ok()) return right.status();
        const auto comparison = ParseComparisonOperator();
        if (!comparison.ok()) return comparison.status();
        const auto literal = ParseLiteral();
        if (!literal.ok()) return literal.status();
        return AstGroupHaving{
            .aggregate = aggregate.value(),
            .column = std::move(column),
            .count_star = count_star,
            .comparison = comparison.value(),
            .literal = literal.value(),
        };
    }

    [[nodiscard]] Result<std::int64_t> ParseIntegerValue(std::string_view what) {
        const SourceSpan start = Current().span;
        bool negative = Match(TokenKind::kMinus);
        const auto token = Expect(TokenKind::kIntegerLiteral, std::string("integer after ") + std::string(what));
        if (!token.ok()) return token.status();
        std::string text = negative ? "-" : "";
        text += token.value().lexeme;
        std::int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            SourceSpan span = token.value().span;
            if (negative) span.begin = start.begin;
            return SyntaxError(span, "integer value is outside the INT range");
        }
        return value;
    }

    [[nodiscard]] Result<AstStatement> ParseStatement() {
        if (Match(TokenKind::kCreate)) {
            if (Match(TokenKind::kTable)) {
                const auto statement = ParseCreateTable();
                if (!statement.ok()) {
                    return statement.status();
                }
                return AstStatement{std::move(statement).value()};
            }
            if (Match(TokenKind::kIndex)) {
                const auto statement = ParseCreateIndex();
                if (!statement.ok()) {
                    return statement.status();
                }
                return AstStatement{std::move(statement).value()};
            }
            return SyntaxError(Current().span, "expected TABLE or INDEX after CREATE");
        }
        if (Match(TokenKind::kInsert)) {
            const auto statement = ParseInsert();
            if (!statement.ok()) {
                return statement.status();
            }
            return AstStatement{std::move(statement).value()};
        }
        if (Match(TokenKind::kSelect)) {
            const auto statement = ParseSelect();
            if (!statement.ok()) {
                return statement.status();
            }
            return AstStatement{std::move(statement).value()};
        }
        if (Match(TokenKind::kExplain)) {
            if (!Match(TokenKind::kSelect)) {
                return SyntaxError(Current().span, "EXPLAIN currently supports SELECT only");
            }
            const auto statement = ParseSelect();
            if (!statement.ok()) return statement.status();
            return AstStatement{AstExplainSelect{.select = std::move(statement).value()}};
        }
        if (Match(TokenKind::kUpdate)) {
            const auto statement = ParseUpdate();
            if (!statement.ok()) {
                return statement.status();
            }
            return AstStatement{std::move(statement).value()};
        }
        if (Match(TokenKind::kDelete)) {
            const auto statement = ParseDelete();
            if (!statement.ok()) {
                return statement.status();
            }
            return AstStatement{std::move(statement).value()};
        }
        if (Match(TokenKind::kBegin)) {
            return AstStatement{AstBegin{}};
        }
        if (Match(TokenKind::kCommit)) {
            return AstStatement{AstCommit{}};
        }
        if (Match(TokenKind::kRollback)) {
            return AstStatement{AstRollback{}};
        }
        return SyntaxError(
            Current().span,
            "expected CREATE, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT, or ROLLBACK statement");
    }

    [[nodiscard]] Result<AstCreateTable> ParseCreateTable() {
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }
        const auto left_paren = Expect(TokenKind::kLeftParen, "'(' after table name");
        if (!left_paren.ok()) {
            return left_paren.status();
        }

        std::vector<AstColumnDefinition> columns;
        while (true) {
            const auto name = ParseIdentifier();
            if (!name.ok()) {
                return name.status();
            }

            ColumnType type = ColumnType::kInt;
            SourceSpan type_span = Current().span;
            if (Match(TokenKind::kInt)) {
                type = ColumnType::kInt;
            } else if (Match(TokenKind::kText)) {
                type = ColumnType::kText;
            } else {
                return SyntaxError(Current().span, "expected INT or TEXT column type");
            }

            columns.push_back(AstColumnDefinition{
                .name = name.value(),
                .type = type,
                .type_span = type_span,
            });
            if (!Match(TokenKind::kComma)) {
                break;
            }
        }

        const auto right_paren = Expect(TokenKind::kRightParen, "')' after column definitions");
        if (!right_paren.ok()) {
            return right_paren.status();
        }
        return AstCreateTable{
            .table_name = table_name.value(),
            .columns = std::move(columns),
        };
    }

    [[nodiscard]] Result<AstCreateIndex> ParseCreateIndex() {
        const auto index_name = ParseIdentifier();
        if (!index_name.ok()) {
            return index_name.status();
        }
        const auto on = Expect(TokenKind::kOn, "ON after index name");
        if (!on.ok()) {
            return on.status();
        }
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }
        const auto left_paren = Expect(TokenKind::kLeftParen, "'(' after table name");
        if (!left_paren.ok()) {
            return left_paren.status();
        }
        const auto column_name = ParseIdentifier();
        if (!column_name.ok()) {
            return column_name.status();
        }
        const auto right_paren = Expect(TokenKind::kRightParen, "')' after index column");
        if (!right_paren.ok()) {
            return right_paren.status();
        }
        return AstCreateIndex{
            .index_name = index_name.value(),
            .table_name = table_name.value(),
            .column_name = column_name.value(),
        };
    }

    [[nodiscard]] Result<AstInsert> ParseInsert() {
        const auto into = Expect(TokenKind::kInto, "INTO after INSERT");
        if (!into.ok()) {
            return into.status();
        }
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }
        const auto values = Expect(TokenKind::kValues, "VALUES after table name");
        if (!values.ok()) {
            return values.status();
        }
        const auto left_paren = Expect(TokenKind::kLeftParen, "'(' after VALUES");
        if (!left_paren.ok()) {
            return left_paren.status();
        }

        std::vector<AstLiteral> literals;
        while (true) {
            const auto literal = ParseLiteral();
            if (!literal.ok()) {
                return literal.status();
            }
            literals.push_back(literal.value());
            if (!Match(TokenKind::kComma)) {
                break;
            }
        }

        const auto right_paren = Expect(TokenKind::kRightParen, "')' after VALUES");
        if (!right_paren.ok()) {
            return right_paren.status();
        }
        return AstInsert{
            .table_name = table_name.value(),
            .values = std::move(literals),
        };
    }

    [[nodiscard]] Result<AstSelect> ParseSelect() {
        std::vector<AstSelectItem> projections;
        while (true) {
            const auto projection = ParseSelectItem();
            if (!projection.ok()) {
                return projection.status();
            }
            projections.push_back(projection.value());
            if (!Match(TokenKind::kComma)) {
                break;
            }
        }

        const auto from = Expect(TokenKind::kFrom, "FROM after projection list");
        if (!from.ok()) {
            return from.status();
        }
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }

        std::optional<AstJoin> join;
        AstJoinKind join_kind = AstJoinKind::kInner;
        bool has_join = false;
        if (Match(TokenKind::kLeft)) {
            join_kind = AstJoinKind::kLeft;
            has_join = true;
            if (!Match(TokenKind::kJoin)) {
                return SyntaxError(Current().span, "expected JOIN after LEFT");
            }
        } else if (Match(TokenKind::kInner)) {
            has_join = true;
            if (!Match(TokenKind::kJoin)) {
                return SyntaxError(Current().span, "expected JOIN after INNER");
            }
        } else if (Match(TokenKind::kJoin)) {
            has_join = true;
        }
        if (has_join) {
            const auto joined_table = ParseIdentifier();
            if (!joined_table.ok()) return joined_table.status();
            const auto on = Expect(TokenKind::kOn, "ON after joined table");
            if (!on.ok()) return on.status();
            const auto left_column = ParseColumnRef();
            if (!left_column.ok()) return left_column.status();
            const auto equal = Expect(TokenKind::kEqual, "'=' in JOIN condition");
            if (!equal.ok()) return equal.status();
            const auto right_column = ParseColumnRef();
            if (!right_column.ok()) return right_column.status();
            join = AstJoin{
                .kind = join_kind,
                .table_name = joined_table.value(),
                .left_column = left_column.value(),
                .right_column = right_column.value(),
            };
        }

        std::optional<AstSelectPredicate> predicate;
        if (Match(TokenKind::kWhere)) {
            const auto column = ParseColumnRef();
            if (!column.ok()) return column.status();
            const auto comparison = ParseComparisonOperator();
            if (!comparison.ok()) return comparison.status();
            const auto literal = ParseLiteral();
            if (!literal.ok()) return literal.status();
            predicate = AstSelectPredicate{
                .column = column.value(),
                .literal = literal.value(),
                .comparison = comparison.value(),
            };
        }

        std::vector<AstColumnRef> group_by;
        if (Match(TokenKind::kGroup)) {
            const auto by = Expect(TokenKind::kBy, "BY after GROUP");
            if (!by.ok()) return by.status();
            while (true) {
                const auto column = ParseColumnRef();
                if (!column.ok()) return column.status();
                group_by.push_back(column.value());
                if (!Match(TokenKind::kComma)) break;
            }
        }

        std::optional<AstGroupHaving> having;
        if (Match(TokenKind::kHaving)) {
            const auto parsed = ParseHaving();
            if (!parsed.ok()) return parsed.status();
            having = parsed.value();
        }

        std::vector<AstOrderBy> order_by;
        if (Match(TokenKind::kOrder)) {
            const auto by = Expect(TokenKind::kBy, "BY after ORDER");
            if (!by.ok()) return by.status();
            while (true) {
                const auto column = ParseColumnRef();
                if (!column.ok()) return column.status();
                const bool ascending = !Match(TokenKind::kDesc);
                if (ascending) static_cast<void>(Match(TokenKind::kAsc));
                order_by.push_back(AstOrderBy{.column = column.value(), .ascending = ascending});
                if (!Match(TokenKind::kComma)) break;
            }
        }

        std::optional<std::int64_t> limit;
        if (Match(TokenKind::kLimit)) {
            const auto parsed = ParseIntegerValue("LIMIT");
            if (!parsed.ok()) return parsed.status();
            limit = parsed.value();
        }

        return AstSelect{
            .projections = std::move(projections),
            .table_name = table_name.value(),
            .predicate = std::move(predicate),
            .join = std::move(join),
            .group_by = std::move(group_by),
            .having = std::move(having),
            .order_by = std::move(order_by),
            .limit = limit,
        };
    }

    [[nodiscard]] Result<AstUpdate> ParseUpdate() {
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }
        const auto set = Expect(TokenKind::kSet, "SET after table name");
        if (!set.ok()) {
            return set.status();
        }
        const auto column_name = ParseIdentifier();
        if (!column_name.ok()) {
            return column_name.status();
        }
        const auto equal = Expect(TokenKind::kEqual, "'=' after SET column");
        if (!equal.ok()) {
            return equal.status();
        }
        const auto value = ParseLiteral();
        if (!value.ok()) {
            return value.status();
        }
        const auto where = Expect(TokenKind::kWhere, "WHERE after SET value");
        if (!where.ok()) {
            return where.status();
        }
        const auto predicate = ParsePredicate();
        if (!predicate.ok()) {
            return predicate.status();
        }
        return AstUpdate{
            .table_name = table_name.value(),
            .column_name = column_name.value(),
            .value = value.value(),
            .predicate = predicate.value(),
        };
    }

    [[nodiscard]] Result<AstDelete> ParseDelete() {
        const auto from = Expect(TokenKind::kFrom, "FROM after DELETE");
        if (!from.ok()) {
            return from.status();
        }
        const auto table_name = ParseIdentifier();
        if (!table_name.ok()) {
            return table_name.status();
        }
        const auto where = Expect(TokenKind::kWhere, "WHERE after table name");
        if (!where.ok()) {
            return where.status();
        }
        const auto predicate = ParsePredicate();
        if (!predicate.ok()) {
            return predicate.status();
        }
        return AstDelete{
            .table_name = table_name.value(),
            .predicate = predicate.value(),
        };
    }

    const std::vector<Token>& tokens_;
    std::size_t position_{0U};
};

}  // namespace

Result<AstStatement> ParseSql(std::string_view sql) {
    const auto tokens = LexSql(sql);
    if (!tokens.ok()) {
        return tokens.status();
    }
    return StatementParser(tokens.value()).Parse();
}

}  // namespace kerndb::parser
