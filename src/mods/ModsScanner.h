#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/LoaderConfig.h"
#include "mods/ModPackage.h"

namespace spl::mods
{
/// One loadable user mod: an .rpf file whose assembly parsed and named entries.
struct DiscoveredMod
{
    std::string name; ///< file stem, e.g. "mycar" for "mycar.rpf"
    std::filesystem::path absolutePath;
    ModPackage package;
};

/// Enumerates mods/*.rpf and parses each assembly.
///
/// Pure filesystem work: nothing is filtered except what cannot be a mod at all.
/// Per-file failures become warnings, never fatal to the scan; a missing folder means
/// the user installed no mods, which is not even a warning.
class ModsScanner
{
public:
    struct Result
    {
        /// In file order, sorted case-insensitively, so the outcome never depends on
        /// the order the filesystem happens to enumerate in.
        std::vector<DiscoveredMod> mods;

        /// Problems worth the user's attention: unreadable archives, missing or broken
        /// assemblies.
        std::vector<std::string> warnings;

        /// Archives skipped only because their extension is not spelled ".rpf" in lower case,
        /// which FiveM does too. Worth telling the user, since the file otherwise looks right.
        std::vector<std::string> ignoredArchives;
    };

    [[nodiscard]] static Result Scan(const std::filesystem::path& modsRoot);

    struct Selection
    {
        std::vector<std::string> disabled;        ///< names dropped by settings.disabled
        std::vector<std::string> missingPriority; ///< priority names no mod has
    };

    /// Drops the mods settings.disabled names, then puts settings.priority first, in that order.
    /// The rest keep Scan's order. Names compare case-insensitively.
    [[nodiscard]] static Selection Select(std::vector<DiscoveredMod>& mods,
                                          const config::ModsSettings& settings);
};
} // namespace spl::mods
