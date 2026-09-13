#include "mods/AssemblyParser.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/Strings.h"

namespace spl::mods
{
namespace
{
constexpr std::size_t kMaxDepth = 64;

/// Position tracking for diagnostics.
struct Cursor
{
    std::string_view source;
    std::size_t pos = 0;
    uint32_t line = 1;
    uint32_t column = 1;

    [[nodiscard]] bool AtEnd() const
    {
        return pos >= source.size();
    }

    [[nodiscard]] char Peek() const
    {
        return AtEnd() ? '\0' : source[pos];
    }

    void Advance()
    {
        if (AtEnd())
        {
            return;
        }
        if (source[pos] == '\n')
        {
            ++line;
            column = 1;
        }
        else
        {
            ++column;
        }
        ++pos;
    }

    bool Consume(std::string_view text)
    {
        if (source.substr(pos, text.size()) != text)
        {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            Advance();
        }
        return true;
    }
};

[[nodiscard]] bool IsNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == ':';
}

void SkipWhitespace(Cursor& cursor)
{
    while (!cursor.AtEnd())
    {
        const char c = cursor.Peek();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        cursor.Advance();
    }
}

[[nodiscard]] std::string DecodeEntities(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] != '&')
        {
            out.push_back(text[i++]);
            continue;
        }
        const std::size_t end = text.find(';', i);
        const std::string_view entity =
            (end == std::string_view::npos) ? text.substr(i) : text.substr(i, end - i + 1);
        if (entity == "&amp;")
        {
            out.push_back('&');
        }
        else if (entity == "&lt;")
        {
            out.push_back('<');
        }
        else if (entity == "&gt;")
        {
            out.push_back('>');
        }
        else if (entity == "&quot;")
        {
            out.push_back('"');
        }
        else if (entity == "&apos;")
        {
            out.push_back('\'');
        }
        else if (entity.size() > 3 && entity[1] == '#' && entity.back() == ';')
        {
            const bool hex = entity[2] == 'x' || entity[2] == 'X';
            unsigned code = 0;
            bool valid = true;
            for (std::size_t d = hex ? 3 : 2; d + 1 < entity.size() && valid; ++d)
            {
                const char digit = entity[d];
                code *= hex ? 16 : 10;
                if (digit >= '0' && digit <= '9')
                {
                    code += static_cast<unsigned>(digit - '0');
                }
                else if (hex && digit >= 'a' && digit <= 'f')
                {
                    code += static_cast<unsigned>(digit - 'a' + 10);
                }
                else if (hex && digit >= 'A' && digit <= 'F')
                {
                    code += static_cast<unsigned>(digit - 'A' + 10);
                }
                else
                {
                    valid = false;
                }
            }
            if (valid && code < 128)
            {
                out.push_back(static_cast<char>(code)); // ASCII is all assemblies use
            }
            else
            {
                out.append(entity);
            }
        }
        else
        {
            out.append(entity); // unknown entities pass through literally
        }
        i += entity.size();
    }
    return out;
}

/// Skips comments, declarations and processing instructions between elements.
void SkipMarkup(Cursor& cursor)
{
    while (true)
    {
        if (cursor.Consume("<!--"))
        {
            while (!cursor.AtEnd() && !cursor.Consume("-->"))
            {
                cursor.Advance();
            }
        }
        else if (cursor.Consume("<?"))
        {
            while (!cursor.AtEnd() && !cursor.Consume("?>"))
            {
                cursor.Advance();
            }
        }
        else if (cursor.Consume("<!"))
        {
            while (!cursor.AtEnd() && cursor.Peek() != '>')
            {
                cursor.Advance();
            }
            cursor.Advance();
        }
        else
        {
            return;
        }
        SkipWhitespace(cursor);
    }
}

struct ElementResult
{
    XmlNode node;
    bool selfClosed = false;
    bool failed = false;
};

ElementResult ParseElement(Cursor& cursor, std::vector<AssemblyDiagnostic>& diagnostics,
                           std::size_t depth);

void ParseChildren(Cursor& cursor, XmlNode& node, std::vector<AssemblyDiagnostic>& diagnostics,
                   std::size_t depth)
{
    while (true)
    {
        SkipMarkup(cursor);
        if (cursor.AtEnd())
        {
            diagnostics.push_back(
                AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line, cursor.column,
                                   "unexpected end of file inside <" + node.name + ">"});
            return;
        }
        if (cursor.Peek() != '<')
        {
            const std::size_t begin = cursor.pos;
            while (!cursor.AtEnd() && cursor.Peek() != '<')
            {
                cursor.Advance();
            }
            node.text += DecodeEntities(cursor.source.substr(begin, cursor.pos - begin));
            continue;
        }
        if (cursor.source.substr(cursor.pos, 2) == "</")
        {
            cursor.Advance();
            cursor.Advance();
            const std::size_t begin = cursor.pos;
            while (!cursor.AtEnd() && IsNameChar(cursor.Peek()))
            {
                cursor.Advance();
            }
            const std::string_view close = cursor.source.substr(begin, cursor.pos - begin);
            SkipWhitespace(cursor);
            cursor.Consume(">");
            if (close != node.name)
            {
                diagnostics.push_back(AssemblyDiagnostic{
                    AssemblyDiagnostic::Severity::Error, cursor.line, cursor.column,
                    "expected </" + node.name + "> but found </" + std::string{close} + ">"});
            }
            return;
        }
        if (cursor.source.substr(cursor.pos, 9) == "<![CDATA[")
        {
            cursor.pos += 9; // columns go stale here; CDATA never appears in assemblies
            const std::size_t begin = cursor.pos;
            while (!cursor.AtEnd() && cursor.source.substr(cursor.pos, 3) != "]]>")
            {
                cursor.Advance();
            }
            node.text.append(cursor.source.substr(begin, cursor.pos - begin));
            cursor.Consume("]]>");
            continue;
        }
        ElementResult child = ParseElement(cursor, diagnostics, depth + 1);
        if (child.failed)
        {
            return;
        }
        node.children.push_back(std::move(child.node));
    }
}

ElementResult ParseElement(Cursor& cursor, std::vector<AssemblyDiagnostic>& diagnostics,
                           std::size_t depth)
{
    ElementResult result;
    result.failed = true;
    cursor.Advance(); // '<'
    const uint32_t line = cursor.line;
    const std::size_t begin = cursor.pos;
    while (!cursor.AtEnd() && IsNameChar(cursor.Peek()))
    {
        cursor.Advance();
    }
    if (cursor.pos == begin)
    {
        diagnostics.push_back(AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line,
                                                 cursor.column, "expected an element name"});
        return result;
    }
    result.node.name.assign(cursor.source.substr(begin, cursor.pos - begin));
    result.node.line = line;

    while (true)
    {
        SkipWhitespace(cursor);
        if (cursor.Consume("/>"))
        {
            result.selfClosed = true;
            result.failed = false;
            return result;
        }
        if (cursor.Consume(">"))
        {
            result.failed = false;
            break;
        }
        if (cursor.AtEnd())
        {
            diagnostics.push_back(
                AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line, cursor.column,
                                   "unexpected end of file inside <" + result.node.name + ">"});
            return result;
        }
        const std::size_t nameBegin = cursor.pos;
        while (!cursor.AtEnd() && IsNameChar(cursor.Peek()))
        {
            cursor.Advance();
        }
        const std::string name{cursor.source.substr(nameBegin, cursor.pos - nameBegin)};
        SkipWhitespace(cursor);
        if (!cursor.Consume("=") || name.empty())
        {
            diagnostics.push_back(
                AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line, cursor.column,
                                   "expected attr=\"value\" in <" + result.node.name + ">"});
            return result;
        }
        SkipWhitespace(cursor);
        const char quote = cursor.Peek();
        if (quote != '"' && quote != '\'')
        {
            diagnostics.push_back(AssemblyDiagnostic{
                AssemblyDiagnostic::Severity::Error, cursor.line, cursor.column,
                "attribute values must be quoted in <" + result.node.name + ">"});
            return result;
        }
        cursor.Advance();
        const std::size_t valueBegin = cursor.pos;
        while (!cursor.AtEnd() && cursor.Peek() != quote)
        {
            cursor.Advance();
        }
        const std::string value =
            DecodeEntities(cursor.source.substr(valueBegin, cursor.pos - valueBegin));
        cursor.Advance(); // closing quote, or end of file
        result.node.attributes.emplace_back(name, value);
    }

    if (depth >= kMaxDepth)
    {
        diagnostics.push_back(AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line,
                                                 cursor.column, "elements nest too deeply"});
        return result;
    }
    ParseChildren(cursor, result.node, diagnostics, depth);
    return result;
}

[[nodiscard]] int ParseInt(std::string_view text)
{
    int value = 0;
    bool negative = false;
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
    {
        negative = text[i] == '-';
        ++i;
    }
    for (; i < text.size(); ++i)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            break;
        }
        value = value * 10 + (text[i] - '0');
    }
    return negative ? -value : value;
}
} // namespace

std::string_view ToString(AssemblyDiagnostic::Severity severity)
{
    switch (severity)
    {
    case AssemblyDiagnostic::Severity::Info:
        return "info";
    case AssemblyDiagnostic::Severity::Warning:
        return "warning";
    case AssemblyDiagnostic::Severity::Error:
        return "error";
    }
    return "unknown";
}

std::optional<XmlNode> AssemblyParser::ParseDocument(std::string_view source,
                                                     std::vector<AssemblyDiagnostic>& diagnostics)
{
    Cursor cursor{source};
    // Real assemblies start with a UTF-8 BOM, which tinyxml2 skips and we must too.
    // UTF-16 is rejected outright: tinyxml2 cannot read it either.
    if (cursor.source.starts_with("\xEF\xBB\xBF"))
    {
        cursor.pos += 3;
        cursor.column += 3;
    }
    if (cursor.source.starts_with("\xFF\xFE") || cursor.source.starts_with("\xFE\xFF"))
    {
        diagnostics.push_back(AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line,
                                                 cursor.column,
                                                 "UTF-16 assemblies are not supported"});
        return std::nullopt;
    }
    SkipWhitespace(cursor);
    SkipMarkup(cursor);
    if (cursor.AtEnd() || cursor.Peek() != '<')
    {
        diagnostics.push_back(AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, cursor.line,
                                                 cursor.column, "no root element found"});
        return std::nullopt;
    }
    ElementResult root = ParseElement(cursor, diagnostics, 0);
    if (root.failed)
    {
        return std::nullopt;
    }
    SkipWhitespace(cursor);
    SkipMarkup(cursor);
    if (!cursor.AtEnd())
    {
        diagnostics.push_back(AssemblyDiagnostic{AssemblyDiagnostic::Severity::Warning, cursor.line,
                                                 cursor.column,
                                                 "ignoring content after the root element"});
    }
    return std::move(root.node);
}

const XmlNode* AssemblyParser::FindChild(const XmlNode& node, std::string_view name)
{
    for (const XmlNode& child : node.children)
    {
        if (child.name == name)
        {
            return &child;
        }
    }
    return nullptr;
}

std::string_view AssemblyParser::AttributeOf(const XmlNode& node, std::string_view name)
{
    for (const auto& [key, value] : node.attributes)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

std::string AssemblyParser::ChildText(const XmlNode& node, std::string_view name)
{
    const XmlNode* child = FindChild(node, name);
    return child != nullptr ? child->text : std::string{};
}

bool AssemblyParser::HasError(const std::vector<AssemblyDiagnostic>& diagnostics)
{
    for (const AssemblyDiagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.severity == AssemblyDiagnostic::Severity::Error)
        {
            return true;
        }
    }
    return false;
}

void AssemblyParser::ParseContentEntries(const XmlNode& node, std::vector<std::string>& roots,
                                         ModPackage& package,
                                         std::vector<AssemblyDiagnostic>& diagnostics)
{
    for (const XmlNode& child : node.children)
    {
        if (child.name == "archive")
        {
            roots.push_back(std::string{AttributeOf(child, "path")});
            ParseContentEntries(child, roots, package, diagnostics);
            roots.pop_back();
        }
        else if (child.name == "add")
        {
            package.entries.push_back(
                ModEntry{.archiveRoots = roots,
                         .sourceFile = std::string{AttributeOf(child, "source")},
                         .targetFile = child.text});
        }
        else
        {
            diagnostics.push_back(
                AssemblyDiagnostic{AssemblyDiagnostic::Severity::Info, child.line, 0,
                                   "ignoring <" + child.name + "> inside <" + node.name + ">"});
        }
    }
}

std::string AssemblyParser::NormalizeGuid(std::string_view id)
{
    std::string text{id};
    if (!text.empty() && text.front() == '{')
    {
        text.erase(text.begin());
    }
    if (!text.empty() && text.back() == '}')
    {
        text.pop_back();
    }
    if (text.size() != 36)
    {
        return "00000000-0000-0000-0000-000000000000";
    }
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = text[i];
        if (dash ? c != '-'
                 : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            return "00000000-0000-0000-0000-000000000000";
        }
    }
    return util::ToUpper(text);
}

AssemblyResult AssemblyParser::ParseAssembly(std::string_view source)
{
    AssemblyResult result;
    std::optional<XmlNode> root = ParseDocument(source, result.diagnostics);
    if (!root)
    {
        result.fatal = true;
        return result;
    }
    if (root->name != "package")
    {
        result.diagnostics.push_back(
            AssemblyDiagnostic{AssemblyDiagnostic::Severity::Error, root->line, 0,
                               "expected <package> but found <" + root->name + ">"});
        result.fatal = true;
        return result;
    }
    if (AttributeOf(*root, "target") != "Five")
    {
        result.diagnostics.push_back(AssemblyDiagnostic{
            AssemblyDiagnostic::Severity::Error, root->line, 0,
            "package targets '" + std::string{AttributeOf(*root, "target")} + "', not 'Five'"});
        result.fatal = true;
        return result;
    }

    result.package.guid = NormalizeGuid(AttributeOf(*root, "id"));
    if (const XmlNode* metadata = FindChild(*root, "metadata"))
    {
        result.package.metadata.name = ChildText(*metadata, "name");
        const XmlNode* version = FindChild(*metadata, "version");
        const int major = version != nullptr ? ParseInt(ChildText(*version, "major")) : 0;
        const int minor = version != nullptr ? ParseInt(ChildText(*version, "minor")) : 0;
        result.package.metadata.version = std::to_string(major) + "." + std::to_string(minor);
        if (const XmlNode* author = FindChild(*metadata, "author"))
        {
            result.package.metadata.authorName = ChildText(*author, "displayName");
        }
        result.package.metadata.description = ChildText(*metadata, "description");
    }
    if (const XmlNode* content = FindChild(*root, "content"))
    {
        std::vector<std::string> roots;
        ParseContentEntries(*content, roots, result.package, result.diagnostics);
    }
    result.fatal = HasError(result.diagnostics);
    return result;
}

Setup2Result AssemblyParser::ParseSetup2(std::string_view source)
{
    Setup2Result result;
    std::optional<XmlNode> root = ParseDocument(source, result.diagnostics);
    if (!root)
    {
        result.fatal = true;
        return result;
    }
    result.descriptor.deviceName = ChildText(*root, "deviceName");
    if (const XmlNode* order = FindChild(*root, "order"))
    {
        // The evident intent of FiveM ModVFSDevice.cpp:486, whose truthiness check on the
        // parsed integer is never taken.
        result.descriptor.order = ParseInt(AttributeOf(*order, "value"));
    }
    result.descriptor.requiredVersion = ChildText(*root, "requiredVersion");
    result.fatal = HasError(result.diagnostics);
    return result;
}

ContentResult AssemblyParser::ParseContent(std::string_view source)
{
    ContentResult result;
    std::optional<XmlNode> root = ParseDocument(source, result.diagnostics);
    if (!root)
    {
        result.fatal = true;
        return result;
    }
    if (const XmlNode* dataFiles = FindChild(*root, "dataFiles"))
    {
        for (const XmlNode& item : dataFiles->children)
        {
            if (item.name != "Item")
            {
                continue;
            }
            result.items.push_back(ContentItem{.filename = ChildText(item, "filename"),
                                               .fileType = ChildText(item, "fileType")});
        }
    }
    result.fatal = HasError(result.diagnostics);
    return result;
}
} // namespace spl::mods
