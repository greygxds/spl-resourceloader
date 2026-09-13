#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spl::manifest
{
enum class TokenKind
{
    Name,       ///< identifier: [A-Za-z_][A-Za-z0-9_]*
    String,     ///< quoted or long string; text holds the decoded value
    Number,     ///< lexed so it can be skipped cleanly
    LeftBrace,  ///< {
    RightBrace, ///< }
    LeftParen,  ///< (
    RightParen, ///< )
    Comma,      ///< ,
    Semicolon,  ///< ;
    Equals,     ///< =
    Other,      ///< punctuation and operators the grammar does not use
    EndOfFile,
    Error ///< text holds the reason; lexing stops here
};

[[nodiscard]] std::string_view ToString(TokenKind kind);

struct Token
{
    TokenKind kind = TokenKind::EndOfFile;
    std::string text;
    uint32_t line = 0;   ///< 1-based
    uint32_t column = 0; ///< 1-based
};

/// Tokenizes the subset of Lua 5.4 lexis that manifests use. It never executes anything and
/// never throws; malformed input becomes an Error token.
///
/// The token stream always ends with EndOfFile or Error.
[[nodiscard]] std::vector<Token> Tokenize(std::string_view source);
} // namespace spl::manifest
