#include "manifest/ManifestParser.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "manifest/LuaManifestLoader.h"
#include "manifest/ManifestLexer.h"
#include "util/Strings.h"

namespace spl::manifest
{
namespace
{
// FiveM refuses to take this key from user metadata and sets it itself after a successful
// fxmanifest.lua (LuaMetaDataLoader.cpp:184-185, :268).
constexpr std::string_view kIsCfxV2Key = "is_cfxv2";

/// Lua keywords that start a construct we do not interpret. Recovery skips past them.
constexpr std::array kBlockOpeners = {"function", "if", "for", "while", "do"};
constexpr std::string_view kBlockCloser = "end";

/// One argument of a manifest call: either a single string or a table of strings.
struct Argument
{
    bool isTable = false;
    std::vector<std::string> values; ///< a string argument holds exactly one
    uint32_t line = 0;
};

class Parser
{
public:
    Parser(std::vector<Token> tokens, std::string_view chunkName)
        : m_tokens(std::move(tokens)), m_chunkName(chunkName)
    {
    }

    ParseResult Run()
    {
        ParseResult result;

        // A lexer error is the one thing that stops us: past it, positions are meaningless.
        if (!m_tokens.empty() && m_tokens.back().kind == TokenKind::Error)
        {
            const Token& error = m_tokens.back();
            result.fatal = true;
            result.diagnostics.push_back(
                ManifestDiagnostic{.severity = ManifestDiagnostic::Severity::Error,
                                   .line = error.line,
                                   .column = error.column,
                                   .message = error.text});
            m_tokens.pop_back();
        }

        m_result = &result;
        while (!AtEnd())
        {
            ParseStatement();
        }
        return result;
    }

private:
    [[nodiscard]] const Token& Current() const
    {
        return m_tokens[m_position];
    }

    [[nodiscard]] const Token& Peek(std::size_t offset = 1) const
    {
        const std::size_t index = m_position + offset;
        return index < m_tokens.size() ? m_tokens[index] : m_tokens.back();
    }

    [[nodiscard]] bool AtEnd() const
    {
        return m_position >= m_tokens.size() || Current().kind == TokenKind::EndOfFile;
    }

    void Advance()
    {
        if (m_position < m_tokens.size())
        {
            ++m_position;
        }
    }

    void Report(ManifestDiagnostic::Severity severity, const Token& token, std::string message)
    {
        m_result->diagnostics.push_back(ManifestDiagnostic{.severity = severity,
                                                           .line = token.line,
                                                           .column = token.column,
                                                           .message = std::move(message)});
    }

    /// Reads `{ 'a', 'b' }`, keeping only the array part, which is what ipairs would walk.
    /// Named fields and nested tables are noted and skipped.
    std::optional<Argument> ParseTable()
    {
        const Token& opening = Current();
        Advance(); // '{'

        Argument argument{.isTable = true, .values = {}, .line = opening.line};
        int depth = 1;

        while (!AtEnd())
        {
            const Token& token = Current();

            if (token.kind == TokenKind::RightBrace)
            {
                Advance();
                if (--depth == 0)
                {
                    return argument;
                }
                continue;
            }
            if (token.kind == TokenKind::LeftBrace)
            {
                Report(ManifestDiagnostic::Severity::Info, token,
                       "Nested table in a manifest table is ignored");
                ++depth;
                Advance();
                continue;
            }

            // `key = value` contributes nothing to ipairs, so skip the whole field.
            if (token.kind == TokenKind::Name && Peek().kind == TokenKind::Equals)
            {
                Report(ManifestDiagnostic::Severity::Info, token,
                       fmt::format("Named table field '{}' is ignored", token.text));
                Advance();
                Advance();
                if (!AtEnd() && Current().kind != TokenKind::Comma &&
                    Current().kind != TokenKind::Semicolon &&
                    Current().kind != TokenKind::RightBrace)
                {
                    Advance();
                }
                continue;
            }

            if (depth == 1 && token.kind == TokenKind::String)
            {
                argument.values.push_back(token.text);
                Advance();
                continue;
            }

            if (token.kind == TokenKind::Number || token.kind == TokenKind::Name)
            {
                Report(ManifestDiagnostic::Severity::Info, token,
                       fmt::format("Non-string table entry '{}' is ignored", token.text));
            }
            Advance();
        }

        Report(ManifestDiagnostic::Severity::Error, opening, "Unterminated table");
        return std::nullopt;
    }

    /// One call argument: a string, a table, or either of those in parentheses.
    std::optional<Argument> ParseArgument()
    {
        const Token& token = Current();

        if (token.kind == TokenKind::String)
        {
            Advance();
            return Argument{.isTable = false, .values = {token.text}, .line = token.line};
        }
        if (token.kind == TokenKind::LeftBrace)
        {
            return ParseTable();
        }
        if (token.kind == TokenKind::LeftParen)
        {
            Advance();
            std::optional<Argument> inner;
            if (Current().kind == TokenKind::String)
            {
                inner =
                    Argument{.isTable = false, .values = {Current().text}, .line = Current().line};
                Advance();
            }
            else if (Current().kind == TokenKind::LeftBrace)
            {
                inner = ParseTable();
            }
            else
            {
                return std::nullopt;
            }

            if (!AtEnd() && Current().kind == TokenKind::RightParen)
            {
                Advance();
            }
            return inner;
        }
        return std::nullopt;
    }

    /// Is this token the start of something we can parse: `name 'string'`, `name {`, `name (`?
    [[nodiscard]] bool LooksLikeCall(std::size_t offset) const
    {
        const std::size_t index = m_position + offset;
        if (index + 1 >= m_tokens.size() || m_tokens[index].kind != TokenKind::Name)
        {
            return false;
        }
        const TokenKind next = m_tokens[index + 1].kind;
        return next == TokenKind::String || next == TokenKind::LeftBrace ||
               next == TokenKind::LeftParen;
    }

    /// Skips an unsupported construct, then resumes at the next thing that looks like a call
    /// on a fresh line. Balanced function/if/do blocks are skipped whole.
    void Recover()
    {
        const uint32_t startLine = Current().line;
        int blockDepth = 0;

        while (!AtEnd())
        {
            const Token& token = Current();

            if (token.kind == TokenKind::Name)
            {
                if (std::ranges::find(kBlockOpeners, token.text) != kBlockOpeners.end())
                {
                    // `for ... do` and `if ... then` open one block between them, not two.
                    if (token.text != "do" || blockDepth == 0)
                    {
                        ++blockDepth;
                    }
                    Advance();
                    continue;
                }
                if (token.text == kBlockCloser)
                {
                    Advance();
                    if (blockDepth > 0 && --blockDepth == 0)
                    {
                        return;
                    }
                    continue;
                }
            }

            if (blockDepth == 0 && token.line > startLine && LooksLikeCall(0))
            {
                return;
            }
            Advance();
        }
    }

    void ParseStatement()
    {
        if (Current().kind == TokenKind::Semicolon)
        {
            Advance();
            return;
        }

        if (!LooksLikeCall(0))
        {
            const Token& token = Current();
            Report(
                ManifestDiagnostic::Severity::Warning, token,
                fmt::format("Unsupported Lua construct '{}' — skipped",
                            token.text.empty() ? std::string{ToString(token.kind)} : token.text));
            Recover();
            return;
        }

        const Token nameToken = Current();
        Advance();

        const std::optional<Argument> first = ParseArgument();
        if (!first)
        {
            Report(ManifestDiagnostic::Severity::Warning, nameToken,
                   fmt::format("Could not read the argument of '{}' — skipped", nameToken.text));
            Recover();
            return;
        }

        // resource_init.lua: the key loses one trailing 's' only when the value is a table.
        const std::string& key = nameToken.text;
        const std::string tableKey =
            (key.size() > 1 && key.back() == 's') ? key.substr(0, key.size() - 1) : key;
        const std::string effectiveKey = first->isTable ? tableKey : key;

        if (first->isTable)
        {
            for (const std::string& value : first->values)
            {
                AddEntry(effectiveKey, value, {value}, first->line);
            }
        }
        else
        {
            AddEntry(effectiveKey, first->values.front(), {first->values.front()}, first->line);
        }

        // A second argument becomes "<key>_extra", JSON-encoded. The key is the de-pluralized
        // one when the FIRST argument was a table, which is what newK holds in Lua.
        if (const std::optional<Argument> second = ParseArgument())
        {
            const std::string extraKey = (first->isTable ? tableKey : key) + "_extra";
            const std::string encoded = second->isTable ? EncodeJsonArray(second->values)
                                                        : EncodeJsonString(second->values.front());
            AddEntry(extraKey, encoded, second->values, second->line);

            // Lua would call the nil returned by the _extra function and abort the manifest.
            // Being lenient is more useful here than matching that failure.
            while (const std::optional<Argument> extra = ParseArgument())
            {
                Report(
                    ManifestDiagnostic::Severity::Warning, nameToken,
                    fmt::format("'{}' has more than two arguments; the extra one is ignored", key));
                if (extra->values.empty())
                {
                    break;
                }
            }
        }
    }

    void AddEntry(const std::string& key, std::string value, std::vector<std::string> decoded,
                  uint32_t line)
    {
        if (util::EqualsIgnoreCase(key, kIsCfxV2Key))
        {
            return; // never taken from user metadata
        }
        m_result->document.Add(ManifestEntry{
            .key = key, .value = std::move(value), .decoded = std::move(decoded), .line = line});
    }

    std::vector<Token> m_tokens;
    std::string_view m_chunkName;
    std::size_t m_position = 0;
    ParseResult* m_result = nullptr;
};
} // namespace

std::string_view ToString(ManifestDiagnostic::Severity severity)
{
    using enum ManifestDiagnostic::Severity;
    switch (severity)
    {
    case Info:
        return "info";
    case Warning:
        return "warning";
    case Error:
        return "error";
    }
    return "warning";
}

void ManifestDocument::Add(ManifestEntry entry)
{
    m_entries.push_back(std::move(entry));
}

std::vector<const ManifestEntry*> ManifestDocument::GetEntries(std::string_view key) const
{
    std::vector<const ManifestEntry*> matches;
    for (const ManifestEntry& entry : m_entries)
    {
        if (entry.key == key)
        {
            matches.push_back(&entry);
        }
    }
    return matches;
}

bool ManifestDocument::Has(std::string_view key) const
{
    return std::ranges::any_of(m_entries,
                               [&](const ManifestEntry& entry) { return entry.key == key; });
}

std::string EncodeJsonString(std::string_view text)
{
    std::string encoded;
    encoded.reserve(text.size() + 2);
    encoded += '"';
    for (const char character : text)
    {
        switch (character)
        {
        case '"':
            encoded += "\\\"";
            break;
        case '\\':
            encoded += "\\\\";
            break;
        case '\n':
            encoded += "\\n";
            break;
        case '\r':
            encoded += "\\r";
            break;
        case '\t':
            encoded += "\\t";
            break;
        case '\b':
            encoded += "\\b";
            break;
        case '\f':
            encoded += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                encoded += fmt::format("\\u{:04x}", static_cast<unsigned char>(character));
            }
            else
            {
                encoded += character;
            }
            break;
        }
    }
    encoded += '"';
    return encoded;
}

std::string EncodeJsonArray(const std::vector<std::string>& values)
{
    std::string encoded = "[";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index > 0)
        {
            encoded += ',';
        }
        encoded += EncodeJsonString(values[index]);
    }
    encoded += ']';
    return encoded;
}

ParseResult ManifestParser::ParseText(std::string_view source, std::string_view chunkName)
{
    return Parser{Tokenize(source), chunkName}.Run();
}

ParseResult ManifestParser::Parse(std::string_view source, std::string_view chunkName)
{
    ParseResult lua = LoadManifestWithLua(source, chunkName);
    if (!lua.fatal)
    {
        return lua;
    }

    // The chunk did not run to completion, so whatever it added is a partial reading of an
    // unknown fraction of the file. The text parser starts over and recovers statement by
    // statement, which yields more than half-executed Lua does.
    ParseResult text = ParseText(source, chunkName);

    std::vector<ManifestDiagnostic> combined;
    combined.reserve(lua.diagnostics.size() + text.diagnostics.size() + 1);
    for (ManifestDiagnostic& diagnostic : lua.diagnostics)
    {
        diagnostic.severity = ManifestDiagnostic::Severity::Warning;
        combined.push_back(std::move(diagnostic));
    }
    combined.push_back(ManifestDiagnostic{.severity = ManifestDiagnostic::Severity::Warning,
                                          .line = 0,
                                          .column = 0,
                                          .message =
                                              "The manifest did not run; it was read as plain text "
                                              "instead, so anything it computed is missing"});
    combined.insert(combined.end(), text.diagnostics.begin(), text.diagnostics.end());
    text.diagnostics = std::move(combined);
    return text;
}
} // namespace spl::manifest
