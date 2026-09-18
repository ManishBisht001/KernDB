#include <algorithm>
#include <cstddef>
#include <string>

#include "kerndb/result.h"
#include "parser/lexer.h"
#include "parser/token.h"
#include "test_framework.h"

KERNDB_TEST(LexerHandlesKeywordsCommentsEscapedStringsAndSpans) {
    const auto tokens = kerndb::parser::LexSql(
        "-- setup\nCREATE TABLE People (id INT, name TEXT); "
        "INSERT INTO people VALUES (1, 'Ada''s');");

    KERNDB_EXPECT(tokens.ok());
    KERNDB_EXPECT_EQ(kerndb::parser::TokenKind::kCreate, tokens.value()[0].kind);
    KERNDB_EXPECT_EQ(std::string("People"), tokens.value()[2].lexeme);
    KERNDB_EXPECT_EQ(std::size_t{2U}, tokens.value()[0].span.begin.line);
    const auto string_token = std::find_if(
        tokens.value().begin(),
        tokens.value().end(),
        [](const kerndb::parser::Token& token) {
            return token.kind == kerndb::parser::TokenKind::kStringLiteral;
        });
    KERNDB_EXPECT(string_token != tokens.value().end());
    KERNDB_EXPECT_EQ(std::string("Ada's"), string_token->lexeme);
}

KERNDB_TEST(LexerReportsUnexpectedAndUnterminatedInput) {
    const auto unexpected = kerndb::parser::LexSql("SELECT @ FROM people");
    KERNDB_EXPECT(!unexpected.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kSyntax, unexpected.status().code());

    const auto unterminated = kerndb::parser::LexSql("INSERT INTO people VALUES ('Ada)");
    KERNDB_EXPECT(!unterminated.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kSyntax, unterminated.status().code());
}
