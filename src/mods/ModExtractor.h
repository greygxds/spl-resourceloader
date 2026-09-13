#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/Result.h"
#include "mods/ModsScanner.h"

namespace spl::mods
{
/// Extracts a discovered mod into its cache folder: stream candidates, map/manifest
/// targets and faux-pack files under stream/, every file the overlay maps under common/ and
/// platform/, plus a synthesized fxmanifest.lua loading the metas whose type is known.
class ModExtractor
{
public:
    /// A folder that replaces game files once mounted over a game mount point, the way FiveM
    /// mounts a mod's common/ and platform/ (ModVFSDevice.cpp:354-389).
    struct OverlayRoot
    {
        std::filesystem::path folder; ///< "<cache>/<mod>/common"
        std::string mountPoint;       ///< "common:/"
        std::string probeFile;        ///< a file extracted there, "data/handling.meta"
    };

    /// A file a content DLC's content.xml names, by the DLC device that serves it.
    struct DlcFile
    {
        std::string device;  ///< lower-case, "vde_laroads"
        std::string path;    ///< lower-case and device-relative, "x64/lods.rpf"
        std::string warning; ///< reported when no mod provides the file; empty when provided
    };

    struct Result
    {
        std::filesystem::path root; ///< the mod's cache folder, created even when empty
        uint32_t extractedFiles = 0;
        bool reused = false; ///< the cache still matched the archive, so nothing was extracted
        std::vector<OverlayRoot> overlays; ///< in mount order; empty when nothing is mapped
        std::vector<std::string> warnings;
        std::vector<DlcFile> providedDlcFiles;   ///< found in one of the mod's own DLC packs
        std::vector<DlcFile> unresolvedDlcFiles; ///< in none of them, perhaps in another mod's
    };

    /// Creates the shared cache folder and removes the folders of mods that are gone. Once per
    /// startup, before extracting.
    [[nodiscard]] static spl::Result<void> PruneCache(const std::filesystem::path& cacheRoot,
                                                      std::span<const DiscoveredMod> mods);

    /// Extracts one mod, or reuses the extraction already in its folder when the archive, the
    /// loader and the game build are what they were then. gameBuild is the running game's build
    /// number (0 when unknown): DLC archives declaring an incompatible requiredVersion are skipped.
    /// Never fails outright; per-file problems become warnings.
    [[nodiscard]] static Result Extract(const DiscoveredMod& mod,
                                        const std::filesystem::path& cacheRoot, uint32_t gameBuild);

    /// The warnings of the unresolved DLC files that no result provides. A DLC split into
    /// dlc.rpf and dlc1.rpf lists both halves in each content.xml, and the halves may ship as
    /// separate mods (VDE LA Roads Part1/Part2), so a file is only missing once every mod is known.
    [[nodiscard]] static std::vector<std::string>
    ReportUnresolvedDlcFiles(std::span<const Result> results);
};
} // namespace spl::mods
