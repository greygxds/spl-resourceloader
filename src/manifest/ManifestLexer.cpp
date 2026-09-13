#include "manifest/ManifestLexer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spl::manifest
{
namespace
{
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

bool IsHexDigit(char character)
{
    return IsDigit(character) || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

bool IsNameStart(char character)
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           character == '_';
}

bool IsNameContinuation(char character)
{
    return IsNameStart(character) || IsDigit(character);
}

int HexValue(char character)
{
    if (IsDigit(character))
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    return character - 'A' + 10;
}

/// Appends a code point as UTF-8, for the \u{XXXX} escape.
void AppendUtf8(std::string& target, uint32_t codePoint)
{
    if (codePoint < 0x80)
    {
        target += static_cast<char>(codePoint);
    }
    else if (codePoint < 0x800)
    {
        target += static_cast<char>(0xC0 | (codePoint >> 6));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint < 0x10000)
    {
        target += static_cast<char>(0xE0 | (codePoint >> 12));
        target += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    else
    {
        target += static_cast<char>(0xF0 | (codePoint >> 18));
        target += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        target += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

/// Walks the source once, tracking line and column so diagnostics can point at the problem.
class Lexer
{
public:
    explicit Lexer(std::string_view source) : m_source(source)
    {
        if (m_source.starts_with(kUtf8Bom))
        {
            m_position = kUtf8Bom.size(); // a BOM is not part of the chunk
        }
    }

    std::vector<Token> Run()
    {
        std::vector<Token> tokens;
        while (true)
        {
            if (!SkipTriviaAndComments(tokens))
            {
                return tokens; // the error token is already the last one
            }

            const uint32_t line = m_line;
            const uint32_t column = m_column;

            if (AtEnd())
            {
                tokens.push_back(
                    Token{.kind = TokenKind::EndOfFile, .line = line, .column = column});
                return tokens;
            }

            const Token token = NextToken(line, column);
            const bool fatal = token.kind == TokenKind::Error;
            tokens.push_back(token);
            if (fatal)
            {
                return tokens;
            }
        }
    }

private:
    [[nodiscard]] bool AtEnd() const
    {
        return m_position >= m_source.size();
    }

    [[nodiscard]] char Peek(std::size_t offset = 0) const
    {
        const std::size_t index = m_position + offset;
        return index < m_source.size() ? m_source[index] : '\0';
    }

    char Advance()
    {
        const char character = m_source[m_position++];
        if (character == '\n')
        {
            ++m_line;
            m_column = 1;
        }
        else
        {
            ++m_column;
        }
        return character;
    }

    Token MakeError(std::string message, uint32_t line, uint32_t column) const
    {
        return Token{
            .kind = TokenKind::Error, .text = std::move(message), .line = line, .column = column};
    }

    /// Consumes whitespace and comments. False when a comment was unterminated, in which case
    /// an Error token has been appended and lexing must stop.
    bool SkipTriviaAndComments(std::vector<Token>& tokens)
    {
        while (!AtEnd())
        {
            const char character = Peek();
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
                character == '\f' || character == '\v')
            {
                Advance();
                continue;
            }

            if (character == '-' && Peek(1) == '-')
            {
                const uint32_t line = m_line;
                const uint32_t column = m_column;
                Advance();
                Advance();

                // --[[ block ]] and --[==[ block ]==], else a line comment.
                if (const std::optional<std::size_t> level = PeekLongBracketLevel())
                {
                    if (!SkipLongBracket(*level))
                    {
                        tokens.push_back(MakeError("Unterminated block comment", line, column));
                        return false;
                    }
                    continue;
                }

                while (!AtEnd() && Peek() != '\n')
                {
                    Advance();
                }
                continue;
            }
            return true;
        }
        return true;
    }

    /// For a `[`, `[=`, `[==` … `[` opener, the number of '=' signs. Does not consume.
    [[nodiscard]] std::optional<std::size_t> PeekLongBracketLevel() const
    {
        if (Peek() != '[')
        {
            return std::nullopt;
        }
        std::size_t level = 0;
        while (Peek(level + 1) == '=')
        {
            ++level;
        }
        return Peek(level + 1) == '[' ? std::optional{level} : std::nullopt;
    }

    /// Consumes an opened long bracket and its contents. Returns false when unterminated.
    /// The text, if wanted, is written to out; a leading newline is dropped, as Lua does.
    bool SkipLongBracket(std::size_t level, std::string* out = nullptr)
    {
        for (std::size_t index = 0; index < level + 2; ++index)
        {
            Advance(); // the opening [==[
        }

        if (Peek() == '\r')
        {
            Advance();
        }
        if (Peek() == '\n')
        {
            Advance(); // a first newline right after the opener is not part of the string
        }

        while (!AtEnd())
        {
            if (Peek() == ']')
            {
                std::size_t closing = 0;
                while (Peek(closing + 1) == '=')
                {
                    ++closing;
                }
                if (closing == level && Peek(closing + 1) == ']')
                {
                    for (std::size_t index = 0; index < level + 2; ++index)
                    {
                        Advance();
                    }
                    return true;
                }
            }

            const char character = Advance();
            if (out != nullptr)
            {
                *out += character;
            }
        }
        return false;
    }

    Token ReadQuotedString(uint32_t line, uint32_t column)
    {
        const char quote = Advance();
        std::string value;

        while (true)
        {
            if (AtEnd())
            {
                return MakeError("Unterminated string", line, column);
            }
            const char character = Peek();
            if (character == '\n')
            {
                return MakeError("Unterminated string", line, column);
            }
            if (character == quote)
            {
                Advance();
                return Token{.kind = TokenKind::String,
                             .text = std::move(value),
                             .line = line,
                             .column = column};
            }
            if (character != '\\')
            {
                value += Advance();
                continue;
            }

            Advance(); // the backslash
            if (AtEnd())
            {
                return MakeError("Unterminated string escape", line, column);
            }

            switch (const char escape = Advance())
            {
            case 'n':
                value += '\n';
                break;
            case 't':
                value += '\t';
                break;
            case 'r':
                value += '\r';
                break;
            case 'a':
                value += '\a';
                break;
            case 'b':
                value += '\b';
                break;
            case 'f':
                value += '\f';
                break;
            case 'v':
                value += '\v';
                break;
            case '\\':
            case '\'':
            case '"':
                value += escape;
                break;
            case '\n':
                value += '\n'; // an escaped newline stands for a newline
                break;
            case 'x':
                if (!IsHexDigit(Peek()) || !IsHexDigit(Peek(1)))
                {
                    return MakeError("Expected two hex digits after \\x", m_line, m_column);
                }
                value += static_cast<char>((HexValue(Advance()) << 4) | HexValue(Advance()));
                break;
            case 'z':
                // \z swallows the following whitespace, newlines included.
                while (!AtEnd() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\r' ||
                                    Peek() == '\n' || Peek() == '\f' || Peek() == '\v'))
                {
                    Advance();
                }
                break;
            case 'u':
            {
                if (Peek() != '{')
                {
                    return MakeError("Expected '{' after \\u", m_line, m_column);
                }
                Advance();
                uint32_t codePoint = 0;
                bool anyDigits = false;
                while (IsHexDigit(Peek()))
                {
                    codePoint = (codePoint << 4) | static_cast<uint32_t>(HexValue(Advance()));
                    anyDigits = true;
                }
                if (!anyDigits || Peek() != '}')
                {
                    return MakeError("Malformed \\u{...} escape", m_line, m_column);
                }
                Advance();
                AppendUtf8(value, codePoint);
                break;
            }
            default:
                if (IsDigit(escape))
                {
                    // \ddd, up to three decimal digits.
                    uint32_t number = static_cast<uint32_t>(escape - '0');
                    for (int index = 0; index < 2 && IsDigit(Peek()); ++index)
                    {
                        number = number * 10 + static_cast<uint32_t>(Advance() - '0');
                    }
                    if (number > 0xFF)
                    {
                        return MakeError("Decimal escape out of range", m_line, m_column);
                    }
                    value += static_cast<char>(number);
                    break;
                }
                return MakeError(std::string{"Unknown string escape '\\"} + escape + "'", m_line,
                                 m_column);
            }
        }
    }

    Token NextToken(uint32_t line, uint32_t column)
    {
        const char character = Peek();

        if (character == '\'' || character == '"')
        {
            return ReadQuotedString(line, column);
        }

        if (const std::optional<std::size_t> level = PeekLongBracketLevel())
        {
            std::string value;
            if (!SkipLongBracket(*level, &value))
            {
                return MakeError("Unterminated long string", line, column);
            }
            return Token{.kind = TokenKind::String,
                         .text = std::move(value),
                         .line = line,
                         .column = column};
        }

        if (IsNameStart(character))
        {
            std::string name;
            while (!AtEnd() && IsNameContinuation(Peek()))
            {
                name += Advance();
            }
            return Token{
                .kind = TokenKind::Name, .text = std::move(name), .line = line, .column = column};
        }

        if (IsDigit(character) || (character == '.' && IsDigit(Peek(1))))
        {
            std::string number;
            while (!AtEnd() && (IsNameContinuation(Peek()) || Peek() == '.' ||
                                ((Peek() == '-' || Peek() == '+') && !number.empty() &&
                                 (number.back() == 'e' || number.back() == 'E'))))
            {
                number += Advance();
            }
            return Token{.kind = TokenKind::Number,
                         .text = std::move(number),
                         .line = line,
                         .column = column};
        }

        Advance();
        const auto simple = [&](TokenKind kind)
        {
            return Token{
                .kind = kind, .text = std::string{character}, .line = line, .column = column};
        };
        switch (character)
        {
        case '{':
            return simple(TokenKind::LeftBrace);
        case '}':
            return simple(TokenKind::RightBrace);
        case '(':
            return simple(TokenKind::LeftParen);
        case ')':
            return simple(TokenKind::RightParen);
        case ',':
            return simple(TokenKind::Comma);
        case ';':
            return simple(TokenKind::Semicolon);
        case '=':
            if (Peek() == '=')
            {
                Advance();
                return Token{
                    .kind = TokenKind::Other, .text = "==", .line = line, .column = column};
            }
            return simple(TokenKind::Equals);
        default:
            return simple(TokenKind::Other);
        }
    }

    std::string_view m_source;
    std::size_t m_position = 0;
    uint32_t m_line = 1;
    uint32_t m_column = 1;
};
} // namespace

std::string_view ToString(TokenKind kind)
{
    using enum TokenKind;
    switch (kind)
    {
    case Name:
        return "name";
    case String:
        return "string";
    case Number:
        return "number";
    case LeftBrace:
        return "'{'";
    case RightBrace:
        return "'}'";
    case LeftParen:
        return "'('";
    case RightParen:
        return "')'";
    case Comma:
        return "','";
    case Semicolon:
        return "';'";
    case Equals:
        return "'='";
    case Other:
        return "token";
    case EndOfFile:
        return "end of file";
    case Error:
        return "error";
    }
    return "token";
}

std::vector<Token> Tokenize(std::string_view source)
{
    return Lexer{source}.Run();
}
} // namespace spl::manifest
