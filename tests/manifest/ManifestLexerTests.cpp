#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "manifest/ManifestLexer.h"

using spl::manifest::Token;
using spl::manifest::Tokenize;
using spl::manifest::TokenKind;

namespace
{
/// The decoded text of the first string token, which is what the escape tests care about.
std::string FirstString(std::string_view source)
{
    for (const Token& token : Tokenize(source))
    {
        if (token.kind == TokenKind::String)
        {
            return token.text;
        }
    }
    return {};
}

std::vector<TokenKind> Kinds(std::string_view source)
{
    std::vector<TokenKind> kinds;
    for (const Token& token : Tokenize(source))
    {
        kinds.push_back(token.kind);
    }
    return kinds;
}
} // namespace

TEST_CASE("ManifestLexer: reads names, strings and punctuation", "[manifest]")
{
    REQUIRE(Kinds("files { 'a' }") == std::vector{TokenKind::Name, TokenKind::LeftBrace,
                                                  TokenKind::String, TokenKind::RightBrace,
                                                  TokenKind::EndOfFile});
}

TEST_CASE("ManifestLexer: both quote styles work", "[manifest]")
{
    REQUIRE(FirstString("x 'single'") == "single");
    REQUIRE(FirstString("x \"double\"") == "double");
    REQUIRE(FirstString("x 'it\\'s'") == "it's");
    REQUIRE(FirstString("x \"say \\\"hi\\\"\"") == "say \"hi\"");
}

TEST_CASE("ManifestLexer: simple escapes decode", "[manifest]")
{
    REQUIRE(FirstString("x 'a\\nb'") == "a\nb");
    REQUIRE(FirstString("x 'a\\tb'") == "a\tb");
    REQUIRE(FirstString("x 'a\\\\b'") == "a\\b");
    REQUIRE(FirstString("x 'a\\rb'") == "a\rb");
}

TEST_CASE("ManifestLexer: numeric and unicode escapes decode", "[manifest]")
{
    REQUIRE(FirstString("x '\\x41'") == "A");
    REQUIRE(FirstString("x '\\65'") == "A");
    REQUIRE(FirstString("x '\\u{48}'") == "H");
    REQUIRE(FirstString("x '\\u{20AC}'") == "\xE2\x82\xAC"); // euro sign, UTF-8
}

TEST_CASE("ManifestLexer: the z escape swallows following whitespace", "[manifest]")
{
    REQUIRE(FirstString("x 'a\\z   \n   b'") == "ab");
}

TEST_CASE("ManifestLexer: long strings work at any level", "[manifest]")
{
    REQUIRE(FirstString("x [[plain]]") == "plain");
    REQUIRE(FirstString("x [==[level two]==]") == "level two");
    // A newline straight after the opener is not part of the string, as in Lua.
    REQUIRE(FirstString("x [[\nfirst line]]") == "first line");
    // Backslashes are literal inside a long string.
    REQUIRE(FirstString("x [[a\\nb]]") == "a\\nb");
}

TEST_CASE("ManifestLexer: comments of every kind are skipped", "[manifest]")
{
    REQUIRE(Kinds("-- a line comment\nname 'x'") ==
            std::vector{TokenKind::Name, TokenKind::String, TokenKind::EndOfFile});
    REQUIRE(Kinds("--[[ block\ncomment ]] name 'x'") ==
            std::vector{TokenKind::Name, TokenKind::String, TokenKind::EndOfFile});
    REQUIRE(Kinds("--[==[ level ]==] name 'x'") ==
            std::vector{TokenKind::Name, TokenKind::String, TokenKind::EndOfFile});
}

TEST_CASE("ManifestLexer: a UTF-8 BOM is not part of the chunk", "[manifest]")
{
    const std::vector<Token> tokens = Tokenize("\xEF\xBB\xBF"
                                               "name 'x'");

    REQUIRE(tokens.front().kind == TokenKind::Name);
    REQUIRE(tokens.front().text == "name");
}

TEST_CASE("ManifestLexer: CRLF line endings are handled", "[manifest]")
{
    const std::vector<Token> tokens = Tokenize("a 'x'\r\nb 'y'\r\n");

    REQUIRE(tokens.front().line == 1);
    REQUIRE(tokens[2].kind == TokenKind::Name);
    REQUIRE(tokens[2].line == 2);
}

TEST_CASE("ManifestLexer: lines and columns are 1-based and tracked", "[manifest]")
{
    const std::vector<Token> tokens = Tokenize("a 'x'\n\n  b 'y'");

    REQUIRE(tokens.front().line == 1);
    REQUIRE(tokens.front().column == 1);
    REQUIRE(tokens[2].line == 3);
    REQUIRE(tokens[2].column == 3);
}

TEST_CASE("ManifestLexer: an unterminated string is an error token", "[manifest]")
{
    const std::vector<Token> tokens = Tokenize("name 'oops");

    REQUIRE(tokens.back().kind == TokenKind::Error);
    REQUIRE(tokens.back().text == "Unterminated string");
}

TEST_CASE("ManifestLexer: a string may not run past the end of its line", "[manifest]")
{
    REQUIRE(Tokenize("name 'oops\nmore'").back().kind == TokenKind::Error);
}

TEST_CASE("ManifestLexer: an unterminated block comment is an error", "[manifest]")
{
    REQUIRE(Tokenize("--[[ never closed").back().kind == TokenKind::Error);
}

TEST_CASE("ManifestLexer: numbers are lexed so they can be skipped", "[manifest]")
{
    REQUIRE(Kinds("x = 1.5e3") == std::vector{TokenKind::Name, TokenKind::Equals, TokenKind::Number,
                                              TokenKind::EndOfFile});
}

TEST_CASE("ManifestLexer: an empty source yields just end of file", "[manifest]")
{
    REQUIRE(Kinds("") == std::vector{TokenKind::EndOfFile});
    REQUIRE(Kinds("   \n\t ") == std::vector{TokenKind::EndOfFile});
}
