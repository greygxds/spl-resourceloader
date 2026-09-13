#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spl::manifest
{
/// One key/value pair, as FiveM's resource_init.lua metatable would have produced it.
struct ManifestEntry
{
    std::string key;
    std::string value;                ///< what FiveM stores; JSON for a *_extra entry
    std::vector<std::string> decoded; ///< the same data already decoded, so no JSON parser is
                                      ///< needed downstream; {value} for an ordinary entry
    uint32_t line = 0;
};

struct ManifestDiagnostic
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

[[nodiscard]] std::string_view ToString(ManifestDiagnostic::Severity severity);

/// The metadata a manifest produced, in the order it was written.
class ManifestDocument
{
public:
    void Add(ManifestEntry entry);

    /// Every entry with this key, in insertion order. The pointers stay valid as long as the
    /// document is not modified.
    [[nodiscard]] std::vector<const ManifestEntry*> GetEntries(std::string_view key) const;
    [[nodiscard]] bool Has(std::string_view key) const;
    [[nodiscard]] std::span<const ManifestEntry> All() const
    {
        return m_entries;
    }

private:
    std::vector<ManifestEntry> m_entries;
};

struct ParseResult
{
    ManifestDocument document;
    std::vector<ManifestDiagnostic> diagnostics;

    /// The lexer gave up, so the document holds only what was read before that point.
    bool fatal = false;
};

/// Reads a manifest without executing Lua, reproducing what FiveM's resource_init.lua
/// metatable produces (data/shared/citizen/scripting/resource_init.lua).
class ManifestParser
{
public:
    /// Runs the manifest in the sandboxed Lua interpreter, which is what FiveM does and so the
    /// only way to be right about manifests that use real Lua. When the chunk will not run at
    /// all, falls back to ParseText, which recovers from what it cannot interpret and still
    /// returns the declarative entries.
    ///
    /// chunkName appears in diagnostics, e.g. "fxmanifest.lua".
    [[nodiscard]] static ParseResult Parse(std::string_view source, std::string_view chunkName);

    /// Reads the manifest as text, without executing anything. Every manifest in practice is
    /// declarative, so this understands almost all of them, and it is the fallback for a chunk
    /// the interpreter rejects.
    [[nodiscard]] static ParseResult ParseText(std::string_view source, std::string_view chunkName);
};

/// JSON-encodes a string the way json.encode would, for a *_extra value.
[[nodiscard]] std::string EncodeJsonString(std::string_view text);

/// JSON-encodes a list of strings as an array, for a *_extra value built from a table.
[[nodiscard]] std::string EncodeJsonArray(const std::vector<std::string>& values);
} // namespace spl::manifest
