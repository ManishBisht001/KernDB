#include "lexer.h"

#include <cctype>
#include <string>
#include <utility>

namespace kerndb::parser {
namespace {

[[nodiscard]] bool IsIdentifierStart(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

[[nodiscard]] bool IsIdentifierPart(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

[[nodiscard]] std::string LowercaseAscii(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        normalized.push_back(
            static_cast<char>(std::tolower(byte)));
    }
    return normalized;
}

[[nodiscard]] TokenKind KeywordKind(std::string_view value) {
    const std::string normalized = LowercaseAscii(value);
    if (normalized == "create") {
        return TokenKind::kCreate;
    }
    if (normalized == "table") {
        return TokenKind::kTable;
    }
    if (normalized == "insert") {
        return TokenKind::kInsert;
    }
    if (normalized == "into") {
        return TokenKind::kInto;
    }
    if (normalized == "values") {
        return TokenKind::kValues;
    }
    if (normalized == "select") {
        return TokenKind::kSelect;
    }
    if (normalized == "from") {
        return TokenKind::kFrom;
    }
    if (normalized == "where") {
        return TokenKind::kWhere;
    }
    if (normalized == "int") {
        return TokenKind::kInt;
    }
    if (normalized == "text") {
        return TokenKind::kText;
    }
    return TokenKind::kIdentifier;
}

[[nodiscard]] Status SyntaxError(SourceLocation location, std::string message) {
    return Status::Error(ErrorCode::kSyntax, std::move(message))
        .WithContext("offset", std::to_string(location.offset))
        .WithContext("line", std::to_string(location.line))
        .WithContext("column", std::to_string(location.column));
}

class Lexer {
public:
    explicit Lexer(std::string_view input)
        : input_(input) {}

    [[nodiscard]] Result<std::vector<Token>> Lex() {
        std::vector<Token> tokens;
        while (!AtEnd()) {
            SkipTrivia();
            if (AtEnd()) {
                break;
            }

            const SourceLocation start = location_;
            const char character = Peek();
            if (IsIdentifierStart(character)) {
                tokens.push_back(LexIdentifierOrKeyword(start));
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
                tokens.push_back(LexInteger(start));
                continue;
            }
            if (character == '\'') {
                const auto string_token = LexString(start);
                if (!string_token.ok()) {
                    return string_token.status();
                }
                tokens.push_back(std::move(string_token).value());
                continue;
            }

            Advance();
            switch (character) {
                case ',':
                    tokens.push_back(Token{TokenKind::kComma, ",", SourceSpan{start, location_}});
                    break;
                case '(':
                    tokens.push_back(Token{TokenKind::kLeftParen, "(", SourceSpan{start, location_}});
                    break;
                case ')':
                    tokens.push_back(Token{TokenKind::kRightParen, ")", SourceSpan{start, location_}});
                    break;
                case ';':
                    tokens.push_back(Token{TokenKind::kSemicolon, ";", SourceSpan{start, location_}});
                    break;
                case '=':
                    tokens.push_back(Token{TokenKind::kEqual, "=", SourceSpan{start, location_}});
                    break;
                case '-':
                    tokens.push_back(Token{TokenKind::kMinus, "-", SourceSpan{start, location_}});
                    break;
                default:
                    return SyntaxError(
                        start,
                        std::string("unexpected character: ") + character);
            }
        }

        tokens.push_back(Token{
            .kind = TokenKind::kEnd,
            .lexeme = "",
            .span = SourceSpan{location_, location_},
        });
        return tokens;
    }

private:
    [[nodiscard]] bool AtEnd() const noexcept {
        return offset_ == input_.size();
    }

    [[nodiscard]] char Peek() const noexcept {
        return input_[offset_];
    }

    [[nodiscard]] char PeekNext() const noexcept {
        return offset_ + 1U < input_.size() ? input_[offset_ + 1U] : '\0';
    }

    void Advance() {
        const char character = input_[offset_];
        ++offset_;
        ++location_.offset;
        if (character == '\n') {
            ++location_.line;
            location_.column = 1U;
        } else {
            ++location_.column;
        }
    }

    void SkipTrivia() {
        while (!AtEnd()) {
            const char character = Peek();
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                Advance();
                continue;
            }
            if (character == '-' && PeekNext() == '-') {
                while (!AtEnd() && Peek() != '\n') {
                    Advance();
                }
                continue;
            }
            return;
        }
    }

    [[nodiscard]] Token LexIdentifierOrKeyword(SourceLocation start) {
        const std::size_t begin_offset = offset_;
        while (!AtEnd() && IsIdentifierPart(Peek())) {
            Advance();
        }
        const std::string lexeme{input_.substr(begin_offset, offset_ - begin_offset)};
        return Token{
            .kind = KeywordKind(lexeme),
            .lexeme = lexeme,
            .span = SourceSpan{start, location_},
        };
    }

    [[nodiscard]] Token LexInteger(SourceLocation start) {
        const std::size_t begin_offset = offset_;
        while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
            Advance();
        }
        return Token{
            .kind = TokenKind::kIntegerLiteral,
            .lexeme = std::string(input_.substr(begin_offset, offset_ - begin_offset)),
            .span = SourceSpan{start, location_},
        };
    }

    [[nodiscard]] Result<Token> LexString(SourceLocation start) {
        Advance();
        std::string value;
        while (!AtEnd()) {
            const char character = Peek();
            if (character == '\'') {
                Advance();
                if (!AtEnd() && Peek() == '\'') {
                    value.push_back('\'');
                    Advance();
                    continue;
                }
                return Token{
                    .kind = TokenKind::kStringLiteral,
                    .lexeme = std::move(value),
                    .span = SourceSpan{start, location_},
                };
            }
            value.push_back(character);
            Advance();
        }
        return SyntaxError(start, "unterminated string literal");
    }

    std::string_view input_;
    std::size_t offset_{0U};
    SourceLocation location_{};
};

}  // namespace

std::string_view TokenKindName(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::kEnd:
            return "end of input";
        case TokenKind::kIdentifier:
            return "identifier";
        case TokenKind::kIntegerLiteral:
            return "integer literal";
        case TokenKind::kStringLiteral:
            return "string literal";
        case TokenKind::kComma:
            return "','";
        case TokenKind::kLeftParen:
            return "'('";
        case TokenKind::kRightParen:
            return "')'";
        case TokenKind::kSemicolon:
            return "';'";
        case TokenKind::kEqual:
            return "'='";
        case TokenKind::kMinus:
            return "'-'";
        case TokenKind::kCreate:
            return "CREATE";
        case TokenKind::kTable:
            return "TABLE";
        case TokenKind::kInsert:
            return "INSERT";
        case TokenKind::kInto:
            return "INTO";
        case TokenKind::kValues:
            return "VALUES";
        case TokenKind::kSelect:
            return "SELECT";
        case TokenKind::kFrom:
            return "FROM";
        case TokenKind::kWhere:
            return "WHERE";
        case TokenKind::kInt:
            return "INT";
        case TokenKind::kText:
            return "TEXT";
    }
    return "unknown";
}

Result<std::vector<Token>> LexSql(std::string_view sql) {
    return Lexer(sql).Lex();
}

}  // namespace kerndb::parser
