#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mods/ModFileTree.h"
#include "mods/ModsScanner.h"

namespace spl::mods
{
/// Lays a discovered mod out as a resource, read straight from its archive: stream candidates,
/// map/manifest targets and faux-pack files under stream/, every file the overlay maps under
/// common/ and platform/, plus a generated fxmanifest.lua loading the metas whose type is known.
/// Nothing is written to disk; ModFileTree serves the layout.
class ModLayout
{
public:
    /// A folder that replaces game files once mounted over a game mount point, the way FiveM
    /// mounts a mod's common/ and platform/ (ModVFSDevice.cpp:354-389).
    struct OverlayRoot
    {
        std::filesystem::path folder; ///< "<mods root>/<mod>/common", which is not on disk
        std::string mountPoint;       ///< "common:/"
        std::string probeFile;        ///< a file the overlay serves, "data/handling.meta"
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
        std::filesystem::path root; ///< "<mods root>/<mod>": the resource root, not on disk
        std::shared_ptr<const ModFileTree> files; ///< never null, empty when the mod is unreadable
        std::vector<OverlayRoot> overlays;        ///< in mount order; empty when nothing is mapped
        std::vector<std::string> warnings;
        std::vector<DlcFile> providedDlcFiles;   ///< found in one of the mod's own DLC packs
        std::vector<DlcFile> unresolvedDlcFiles; ///< in none of them, perhaps in another mod's
    };

    /// Lays one mod out under modsRoot. gameBuild is the running game's build number (0 when
    /// unknown): DLC archives declaring an incompatible requiredVersion are skipped. Never fails
    /// outright; per-file problems become warnings.
    [[nodiscard]] static Result Build(const DiscoveredMod& mod,
                                      const std::filesystem::path& modsRoot, uint32_t gameBuild);

    /// The warnings of the unresolved DLC files that no result provides. A DLC split into
    /// dlc.rpf and dlc1.rpf lists both halves in each content.xml, and the halves may ship as
    /// separate mods (VDE LA Roads Part1/Part2), so a file is only missing once every mod is known.
    [[nodiscard]] static std::vector<std::string>
    ReportUnresolvedDlcFiles(std::span<const Result> results);
};
} // namespace spl::mods
