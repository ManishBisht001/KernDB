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
        return SyntaxError(Current().span, "expected CREATE, INSERT, or SELECT statement");
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
        std::vector<AstIdentifier> projections;
        while (true) {
            const auto projection = ParseIdentifier();
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

        std::optional<AstPredicate> predicate;
        if (Match(TokenKind::kWhere)) {
            const auto column_name = ParseIdentifier();
            if (!column_name.ok()) {
                return column_name.status();
            }
            const auto equal = Expect(TokenKind::kEqual, "'=' in WHERE predicate");
            if (!equal.ok()) {
                return equal.status();
            }
            const auto literal = ParseLiteral();
            if (!literal.ok()) {
                return literal.status();
            }
            predicate = AstPredicate{
                .column_name = column_name.value(),
                .literal = literal.value(),
            };
        }

        return AstSelect{
            .projections = std::move(projections),
            .table_name = table_name.value(),
            .predicate = std::move(predicate),
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
