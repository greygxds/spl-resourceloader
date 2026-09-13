#include "mods/ModOverlay.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mods/ModPackage.h"
#include "util/Strings.h"

namespace spl::mods
{
namespace
{
/// Targets FiveM never serves, whatever the mod claims (ModVFSDevice.cpp:201-206).
constexpr std::array<std::string_view, 3> kDeniedTargets{"common/data/gameconfig.xml",
                                                         "common/data/ai/scenarios.meta",
                                                         "common/data/ai/conditionalanims.meta"};

/// Metadata extensions M4 can turn into data-file jobs. Everything else that is mapped
/// either streams or waits for the M6 device.
constexpr std::array<std::string_view, 3> kMetaExtensions{".meta", ".dat", ".xml"};

[[nodiscard]] std::string NormalizeSlashes(std::string_view path)
{
    std::string out{path};
    std::ranges::replace(out, '\\', '/');
    return out;
}

/// Archive roots compare normalized: real packages spell them with backslashes (which is
/// what FiveM compares literally), and nothing legitimate differs by slash direction.
[[nodiscard]] std::string NormalizeRoot(std::string_view root)
{
    return NormalizeSlashes(root);
}

/// Maps one entry's target through the root switch, or nullopt to ignore the entry.
/// FiveM ModVFSDevice.cpp:214-235.
[[nodiscard]] std::optional<std::string> RewriteTarget(std::string_view lastRoot,
                                                       std::string_view target)
{
    if (lastRoot == "update/update.rpf")
    {
        if (target.starts_with("x64/"))
        {
            return "platform/" + std::string{target.substr(4)};
        }
        if (target.starts_with("dlc_patch/"))
        {
            return std::nullopt;
        }
        return std::string{target};
    }
    if (lastRoot.size() == 8 && lastRoot.starts_with("x64") && lastRoot.ends_with(".rpf"))
    {
        return "platform/" + std::string{target};
    }
    if (lastRoot == "common.rpf")
    {
        return "common/" + std::string{target};
    }
    return std::nullopt;
}

[[nodiscard]] bool IsDenied(std::string_view target)
{
    return std::ranges::find(kDeniedTargets, target) != kDeniedTargets.end();
}

[[nodiscard]] bool HasExtension(std::string_view file, std::string_view extension)
{
    return file.size() >= extension.size() &&
           util::EqualsIgnoreCase(file.substr(file.size() - extension.size()), extension);
}

[[nodiscard]] bool IsMetaTarget(std::string_view target)
{
    return std::ranges::any_of(kMetaExtensions, [target](std::string_view extension)
                               { return HasExtension(target, extension); });
}

/// The MountModStream predicate, exactly: deeply nested or core texture, and either a
/// bare file name or a core texture (ModVFSDevice.cpp:419-426).
[[nodiscard]] bool IsStreamCandidate(const ModEntry& entry, std::string_view target)
{
    const bool isCoreTexture = target.starts_with("textures/");
    const bool nested = entry.archiveRoots.size() >= 2;
    const bool bare = target.find('/') == std::string_view::npos;
    return (nested || isCoreTexture) && (bare || isCoreTexture);
}
} // namespace

ModOverlay ModOverlay::Build(const ModPackage& package)
{
    ModOverlay overlay;
    // Pass 1, the constructor rules: denied, nested archives and the root switch decide
    // the target→source map (ModVFSDevice.cpp:174-238).
    std::vector<std::optional<std::string>> mappedTargets;
    mappedTargets.reserve(package.entries.size());
    for (const ModEntry& entry : package.entries)
    {
        if (entry.archiveRoots.empty())
        {
            overlay.m_dlcJobs.push_back(DlcJob{.source = NormalizeSlashes(entry.sourceFile)});
            mappedTargets.push_back(std::nullopt);
            continue;
        }
        std::string target = NormalizeSlashes(entry.targetFile);
        if (!target.empty() && target.front() == '/')
        {
            target.erase(target.begin());
        }
        const std::string source = NormalizeSlashes(entry.sourceFile);
        if (IsDenied(target))
        {
            mappedTargets.push_back(std::nullopt);
            continue;
        }
        if (HasExtension(target, ".rpf"))
        {
            overlay.m_fauxPackJobs.push_back(FauxPackJob{.source = source});
            mappedTargets.push_back(std::nullopt);
            continue;
        }
        const std::optional<std::string> mapped =
            RewriteTarget(NormalizeRoot(entry.archiveRoots.back()), target);
        if (!mapped)
        {
            mappedTargets.push_back(std::nullopt);
            continue;
        }
        mappedTargets.push_back(*mapped);
        overlay.m_mappings.push_back(Mapping{.target = *mapped, .source = source});
        if (IsMetaTarget(*mapped))
        {
            overlay.m_metaFiles.push_back(MetaFile{.source = source, .target = *mapped});
        }
        if (HasExtension(*mapped, ".ymap") || HasExtension(*mapped, ".ybn"))
        {
            overlay.m_mapTargets.push_back(Mapping{.target = *mapped, .source = source});
        }
        if (HasExtension(*mapped, ".ymf"))
        {
            overlay.m_manifestTargets.push_back(Mapping{.target = *mapped, .source = source});
        }
    }

    // Pass 2, the MountModStream rules: the predicate runs over every entry with roots,
    // whether it mapped or not (ModVFSDevice.cpp:391-439). Candidates carry the rewritten
    // target when there is one, else the raw normalized target.
    for (std::size_t i = 0; i < package.entries.size(); ++i)
    {
        const ModEntry& entry = package.entries[i];
        if (entry.archiveRoots.empty())
        {
            continue;
        }
        std::string target = NormalizeSlashes(entry.targetFile);
        if (!target.empty() && target.front() == '/')
        {
            target.erase(target.begin());
        }
        if (!IsStreamCandidate(entry, target))
        {
            continue;
        }
        const std::string source = NormalizeSlashes(entry.sourceFile);
        overlay.m_streamCandidates.push_back(
            StreamCandidate{.source = source, .target = mappedTargets[i].value_or(target)});
    }
    return overlay;
}

std::optional<std::string> ModOverlay::FindSource(std::string_view target) const
{
    for (const Mapping& mapping : m_mappings)
    {
        if (mapping.target == target)
        {
            return mapping.source;
        }
    }
    for (const Mapping& mapping : m_mappings)
    {
        if (util::EqualsIgnoreCase(mapping.target, target))
        {
            return mapping.source;
        }
    }
    return std::nullopt;
}
} // namespace spl::mods
