#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "manifest/ManifestParser.h"
#include "util/FileTree.h"

namespace spl::manifest
{
/// One `data_file 'TYPE' 'pattern'` pair, with the pattern already resolved against the
/// resource root.
struct DataFileEntry
{
    std::string type;                  ///< upper-cased, e.g. "DLC_ITYP_REQUEST"
    std::string pattern;               ///< as written, wildcards included, without an @resource/
    std::vector<std::string> resolved; ///< relative paths with forward slashes
    uint32_t line = 0;

    /// "other" for '@other/data/x.meta': the pattern is resolved against that resource's root
    /// when the plan is built, because only the plan knows the other resources.
    std::string otherResource;
};

/// The typed reading of a manifest: what the loader actually acts on.
class ResourceManifest
{
public:
    /// Interprets doc, resolving data_file globs under resourceRoot and appending anything
    /// noteworthy to diagnostics.
    [[nodiscard]] static ResourceManifest
    FromDocument(const ManifestDocument& document, const std::filesystem::path& resourceRoot,
                 std::vector<ManifestDiagnostic>& diagnostics);

    /// The same, with the globs resolved in files rather than on disk.
    [[nodiscard]] static ResourceManifest
    FromDocument(const ManifestDocument& document, const util::IFileTree& files,
                 const std::filesystem::path& resourceRoot,
                 std::vector<ManifestDiagnostic>& diagnostics);

    std::string fxVersion;          ///< informational
    std::vector<std::string> games; ///< lower-cased; empty means unspecified
    bool isMap = false;             ///< any this_is_a_map entry; the value is ignored
    std::vector<DataFileEntry> dataFiles;

    /// Level metas, as written: loaded before the map (init_meta), and around the level's own
    /// list of data files. They only take effect when the loader starts with the game.
    std::vector<std::string> initMetas;
    std::vector<std::string> beforeLevelMetas;
    std::vector<std::string> afterLevelMetas;

    uint32_t ignoredScriptEntries = 0;        ///< scripts, exports, UI: counted, not loaded
    std::vector<std::string> unsupportedKeys; ///< keys with SP meaning we do not implement yet

    // Informational, straight from the manifest.
    std::string name;
    std::string author;
    std::string description;
    std::string version;

    /// True when the manifest does not exclude GTA V: either it names no game, or it names
    /// gta5 or common among them.
    [[nodiscard]] bool IsCompatibleWithGta5() const;

    /// "gta5, rdr3", for a diagnostic.
    [[nodiscard]] std::string DescribeGames() const;
};

/// True for a key that only matters to FiveM's script runtime, which this loader has none of.
[[nodiscard]] bool IsScriptKey(std::string_view key);
} // namespace spl::manifest
