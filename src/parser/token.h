#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kerndb::parser {

struct SourceLocation {
    std::size_t offset{0U};
    std::size_t line{1U};
    std::size_t column{1U};

    [[nodiscard]] bool operator==(const SourceLocation&) const = default;
};

struct SourceSpan {
    SourceLocation begin;
    SourceLocation end;

    [[nodiscard]] bool operator==(const SourceSpan&) const = default;
};

enum class TokenKind {
    kEnd,
    kIdentifier,
    kIntegerLiteral,
    kStringLiteral,
    kComma,
    kLeftParen,
    kRightParen,
    kSemicolon,
    kEqual,
    kMinus,
    kCreate,
    kTable,
    kInsert,
    kInto,
    kValues,
    kSelect,
    kFrom,
    kWhere,
    kInt,
    kText,
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    SourceSpan span;
};

[[nodiscard]] std::string_view TokenKindName(TokenKind kind) noexcept;

}  // namespace kerndb::parser
