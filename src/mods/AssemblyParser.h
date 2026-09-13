#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Result.h"
#include "mods/ModPackage.h"

namespace spl::mods
{
struct AssemblyDiagnostic
{
    enum class Severity
    {
        Info,
        Warning,
        Error
    };

    Severity severity = Severity::Warning;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string message;
};

[[nodiscard]] std::string_view ToString(AssemblyDiagnostic::Severity severity);

/// One parsed element: name, attributes, direct text and children. Public so the
/// parser's helpers can live in the .cpp without friending the class.
struct XmlNode
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<XmlNode> children;
    uint32_t line = 0;
};

/// A parsed assembly.xml plus anything noteworthy. fatal means the document holds only
/// what was read before the parser gave up (mirrors manifest::ParseResult).
struct AssemblyResult
{
    ModPackage package;
    std::vector<AssemblyDiagnostic> diagnostics;
    bool fatal = false;
};

/// A parsed setup2.xml (M5 wires it to the pseudo-DLC path).
struct Setup2Result
{
    DlcDescriptor descriptor;
    std::vector<AssemblyDiagnostic> diagnostics;
    bool fatal = false;
};

/// Parsed content.xml items (M5 wires them to data-file jobs).
struct ContentResult
{
    std::vector<ContentItem> items;
    std::vector<AssemblyDiagnostic> diagnostics;
    bool fatal = false;
};

/// Reads the XML subset OpenIV packages use: elements, attributes, text, comments and
/// the standard entities. Anything else (declarations, doctypes, CDATA, processing
/// instructions) is skipped, because real assembly files only carry comments and an
/// optional declaration. Never throws; malformed input ends the result as fatal.
class AssemblyParser
{
public:
    /// package/archive/add, the mod's file map (FiveM ModPackage.cpp:74-114).
    [[nodiscard]] static AssemblyResult ParseAssembly(std::string_view source);

    /// deviceName/order/requiredVersion of a DLC-style mod (ModVFSDevice.cpp:478-499).
    [[nodiscard]] static Setup2Result ParseSetup2(std::string_view source);

    /// dataFiles/Item list of a DLC-style mod (ModVFSDevice.cpp:559-587).
    [[nodiscard]] static ContentResult ParseContent(std::string_view source);

private:
    /// Parses one document element, or nullopt when there is nothing (left) to parse.
    /// Errors append to diagnostics and set fatal; the tree holds what parsed so far.
    [[nodiscard]] static std::optional<XmlNode>
    ParseDocument(std::string_view source, std::vector<AssemblyDiagnostic>& diagnostics);

    [[nodiscard]] static const XmlNode* FindChild(const XmlNode& node, std::string_view name);
    [[nodiscard]] static std::string_view AttributeOf(const XmlNode& node, std::string_view name);
    [[nodiscard]] static std::string ChildText(const XmlNode& node, std::string_view name);
    [[nodiscard]] static bool HasError(const std::vector<AssemblyDiagnostic>& diagnostics);

    static void ParseContentEntries(const XmlNode& node, std::vector<std::string>& roots,
                                    ModPackage& package,
                                    std::vector<AssemblyDiagnostic>& diagnostics);
    [[nodiscard]] static std::string NormalizeGuid(std::string_view id);
};
} // namespace spl::mods
