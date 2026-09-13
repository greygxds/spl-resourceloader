#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mods/ModPackage.h"

namespace spl::mods
{
/// The game-visible meaning of a mod's entries: FiveM ModVFSDevice.cpp:174-238 (rewrite
/// rules) and :391-439 (streaming heuristic), reimplemented without the VFS.
///
/// Pure data: paths use forward slashes, sources are archive-relative as written.
class ModOverlay
{
public:
    /// A rewritten target and the archive-relative source behind it.
    struct Mapping
    {
        std::string target; ///< e.g. "platform/textures/car.ytd"
        std::string source; ///< e.g. "content/car.ytd"
    };

    /// A mapped entry the streaming collection must also register (the exact predicate
    /// from ModVFSDevice.cpp:421-426, evaluated over every entry with roots whether it
    /// mapped or not). target is the rewritten form when the entry mapped, else the raw
    /// normalized target.
    struct StreamCandidate
    {
        std::string source;
        std::string target;
    };

    /// A mapped metadata file (.meta/.dat/.xml). Served through the overlay like every
    /// mapping; the extractor also loads it as a data file when its name gives the type.
    struct MetaFile
    {
        std::string source;
        std::string target;
    };

    /// A nested .rpf target: open and enumerate it instead of mapping it.
    struct FauxPackJob
    {
        std::string source;
    };

    /// An entry with no archive roots: an outer DLC archive for the deferred M5 path.
    struct DlcJob
    {
        std::string source;
    };

    [[nodiscard]] static ModOverlay Build(const ModPackage& package);

    [[nodiscard]] const std::vector<Mapping>& Mappings() const
    {
        return m_mappings;
    }

    [[nodiscard]] const std::vector<StreamCandidate>& StreamCandidates() const
    {
        return m_streamCandidates;
    }

    [[nodiscard]] const std::vector<MetaFile>& MetaFiles() const
    {
        return m_metaFiles;
    }

    [[nodiscard]] const std::vector<FauxPackJob>& FauxPackJobs() const
    {
        return m_fauxPackJobs;
    }

    [[nodiscard]] const std::vector<DlcJob>& DlcJobs() const
    {
        return m_dlcJobs;
    }

    /// Mapped map/collision targets; M4 feeds them to the map-store reload like any
    /// resource's .ymap/.ybn.
    [[nodiscard]] const std::vector<Mapping>& MapTargets() const
    {
        return m_mapTargets;
    }

    /// Mapped .ymf targets; M4 feeds them to the packfile-manifest loader.
    [[nodiscard]] const std::vector<Mapping>& ManifestTargets() const
    {
        return m_manifestTargets;
    }

    /// The archive source behind a rewritten target, exact first then case-insensitive.
    [[nodiscard]] std::optional<std::string> FindSource(std::string_view target) const;

private:
    std::vector<Mapping> m_mappings;
    std::vector<StreamCandidate> m_streamCandidates;
    std::vector<MetaFile> m_metaFiles;
    std::vector<FauxPackJob> m_fauxPackJobs;
    std::vector<DlcJob> m_dlcJobs;
    std::vector<Mapping> m_mapTargets;
    std::vector<Mapping> m_manifestTargets;
};
} // namespace spl::mods
