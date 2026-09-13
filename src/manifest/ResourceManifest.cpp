#include "manifest/ResourceManifest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "manifest/ManifestParser.h"
#include "util/Glob.h"
#include "util/Strings.h"

namespace spl::manifest
{
namespace
{
// Keys that exist only for FiveM's script runtime. We count them and move on.
constexpr std::array kScriptKeys = {
    "client_script",     "client_scripts", "server_script", "server_scripts",
    "shared_script",     "shared_scripts", "script",        "scripts",
    "ui_page",           "loadscreen",     "export",        "exports",
    "server_export",     "server_exports", "dependency",    "dependencies",
    "provide",           "lua54",          "node_version",  "loadscreen_manual_shutdown",
    "loadscreen_cursor",
};

// Keys that do mean something in single player but that the loader does not implement.
struct UnsupportedKey
{
    std::string_view key;
    std::string_view note;
};
constexpr std::array kUnsupportedKeys = {
    UnsupportedKey{"replace_level_meta",
                   "replacing the whole level would take story mode's map away"},
};

constexpr std::string_view kGameGta5 = "gta5";
constexpr std::string_view kGameCommon = "common";

/// The first value of the first entry with this key, or an empty string.
std::string FirstValue(const ManifestDocument& document, std::string_view key)
{
    const std::vector<const ManifestEntry*> entries = document.GetEntries(key);
    return entries.empty() ? std::string{} : entries.front()->value;
}

void Report(std::vector<ManifestDiagnostic>& diagnostics, ManifestDiagnostic::Severity severity,
            uint32_t line, std::string message)
{
    diagnostics.push_back(ManifestDiagnostic{
        .severity = severity, .line = line, .column = 0, .message = std::move(message)});
}
} // namespace

bool IsScriptKey(std::string_view key)
{
    return std::ranges::find(kScriptKeys, key) != kScriptKeys.end();
}

bool ResourceManifest::IsCompatibleWithGta5() const
{
    return games.empty() ||
           std::ranges::any_of(games, [](const std::string& game)
                               { return game == kGameGta5 || game == kGameCommon; });
}

std::string ResourceManifest::DescribeGames() const
{
    std::string text;
    for (const std::string& game : games)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += game;
    }
    return text;
}

ResourceManifest ResourceManifest::FromDocument(const ManifestDocument& document,
                                                const std::filesystem::path& resourceRoot,
                                                std::vector<ManifestDiagnostic>& diagnostics)
{
    ResourceManifest manifest;

    manifest.fxVersion = FirstValue(document, "fx_version");
    manifest.name = FirstValue(document, "name");
    manifest.author = FirstValue(document, "author");
    manifest.description = FirstValue(document, "description");
    manifest.version = FirstValue(document, "version");

    // Both `game 'gta5'` and `games { ... }` land under "game", because the metatable strips
    // the plural only for table arguments.
    for (const ManifestEntry* entry : document.GetEntries("game"))
    {
        manifest.games.push_back(util::ToLower(entry->value));
    }

    manifest.isMap = document.Has("this_is_a_map");

    // Paths relative to the resource root, handed to the game's level loader during startup
    // (FiveM ResourcesTest.cpp:176-189).
    const auto collectMetas = [&document](std::string_view key, std::vector<std::string>& metas)
    {
        for (const ManifestEntry* entry : document.GetEntries(key))
        {
            metas.push_back(entry->value);
        }
    };
    collectMetas("init_meta", manifest.initMetas);
    collectMetas("before_level_meta", manifest.beforeLevelMetas);
    collectMetas("after_level_meta", manifest.afterLevelMetas);

    // data_file and data_file_extra are two parallel lists. FiveM drops the whole list when
    // their lengths disagree (ResourcesTest.cpp:203-207), because pairing them up is then
    // guesswork, and a wrongly paired data file is worse than none.
    const std::vector<const ManifestEntry*> types = document.GetEntries("data_file");
    const std::vector<const ManifestEntry*> paths = document.GetEntries("data_file_extra");
    if (types.size() != paths.size())
    {
        Report(diagnostics, ManifestDiagnostic::Severity::Error,
               types.empty() ? 0 : types.front()->line,
               fmt::format("data_file entry count mismatch ({} types, {} paths); no data files "
                           "are loaded from this resource",
                           types.size(), paths.size()));
    }
    else
    {
        for (std::size_t index = 0; index < types.size(); ++index)
        {
            const ManifestEntry& type = *types[index];
            const ManifestEntry& path = *paths[index];
            if (path.decoded.empty())
            {
                continue; // json.encode gave something that is not a string
            }

            const std::string& pattern = path.decoded.front();
            if (pattern.starts_with('@'))
            {
                const std::size_t slash = pattern.find('/');
                if (slash == std::string::npos || slash == 1 || slash + 1 == pattern.size())
                {
                    Report(diagnostics, ManifestDiagnostic::Severity::Warning, path.line,
                           fmt::format("data_file '{}' names '{}', which is not "
                                       "'@resource/path'",
                                       type.value, pattern));
                    continue;
                }
                manifest.dataFiles.push_back(
                    DataFileEntry{.type = util::ToUpper(type.value),
                                  .pattern = pattern.substr(slash + 1),
                                  .line = type.line,
                                  .otherResource = pattern.substr(1, slash - 1)});
                continue;
            }

            std::vector<std::string> resolved = util::GlobFiles(resourceRoot, pattern);

            // A wildcard-free pattern that matches nothing is kept as written: some data-file
            // types take a name rather than a path (ResourcesTest.cpp:228-231).
            if (resolved.empty() && util::IsLiteralPattern(pattern))
            {
                resolved.push_back(pattern);
            }
            if (resolved.empty())
            {
                Report(diagnostics, ManifestDiagnostic::Severity::Warning, path.line,
                       fmt::format("data_file pattern '{}' matched no files", pattern));
                continue;
            }

            manifest.dataFiles.push_back(DataFileEntry{.type = util::ToUpper(type.value),
                                                       .pattern = pattern,
                                                       .resolved = std::move(resolved),
                                                       .line = type.line});
        }
    }

    for (const ManifestEntry& entry : document.All())
    {
        if (IsScriptKey(entry.key))
        {
            ++manifest.ignoredScriptEntries;
            continue;
        }

        const auto unsupported =
            std::ranges::find_if(kUnsupportedKeys, [&](const UnsupportedKey& candidate)
                                 { return candidate.key == entry.key; });
        if (unsupported != kUnsupportedKeys.end())
        {
            Report(diagnostics, ManifestDiagnostic::Severity::Warning, entry.line,
                   fmt::format("'{}' is not supported: {}", entry.key, unsupported->note));
            manifest.unsupportedKeys.emplace_back(entry.key);
        }
    }

    return manifest;
}
} // namespace spl::manifest
