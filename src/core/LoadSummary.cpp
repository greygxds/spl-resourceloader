#include "core/LoadSummary.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "manifest/ResourceManifest.h"
#include "streaming/DataFileType.h"
#include "streaming/StreamAsset.h"

namespace spl
{
namespace
{
/// What one resource or mod came to.
struct Tally
{
    std::size_t registered = 0;
    std::size_t replacing = 0; ///< registered assets that took over a game asset
    std::size_t notRegistered = 0;
    std::size_t invalid = 0;
    std::size_t switchedOff = 0;
    std::size_t shadowed = 0;
    std::size_t dataFiles = 0;
    std::size_t dataFilesFailed = 0;
    std::size_t dataFilesRefused = 0;
    std::size_t manifests = 0;
    std::size_t levelMetas = 0;
    std::size_t scriptEntries = 0;
    bool map = false;
    std::vector<std::string> mountPoints; ///< mods only, "common:/" and the like
};

/// "(20 replace game files)", or nothing.
std::string ReplacingNote(std::size_t count)
{
    if (count == 0)
    {
        return {};
    }
    return count == 1 ? std::string{" (1 replaces a game file)"}
                      : fmt::format(" ({} replace game files)", count);
}

/// "common:/ and platform:/", from the mount points without their crc twins.
std::string JoinMountPoints(const std::vector<std::string>& mountPoints)
{
    std::string text;
    for (std::size_t index = 0; index < mountPoints.size(); ++index)
    {
        if (index > 0)
        {
            text += index + 1 == mountPoints.size() ? " and " : ", ";
        }
        text += mountPoints[index];
    }
    return text;
}

Tally Count(const LoadSummaryInput& input, std::size_t index, const resource::Resource& resource)
{
    Tally tally;
    const resource::ResourceId owner{static_cast<uint16_t>(index)};

    for (const streaming::RegisteredAsset& asset : input.registry->All())
    {
        if (asset.owner == owner)
        {
            ++tally.registered;
            tally.replacing += asset.overridesGameAsset ? 1 : 0;
        }
    }
    for (const streaming::StreamAsset& asset : input.plan->Assets())
    {
        if (asset.owner != owner)
        {
            continue;
        }
        using enum streaming::AssetDisposition;
        tally.invalid += asset.disposition == SkippedInvalid ? 1 : 0;
        tally.switchedOff += asset.disposition == SkippedByConfig ? 1 : 0;
        tally.shadowed += asset.disposition == ShadowedByDuplicate ? 1 : 0;
    }
    std::size_t planned = 0;
    for (const streaming::ResourcePlan& plan : input.plan->Resources())
    {
        if (plan.owner == owner)
        {
            planned = plan.plannedAssets;
            tally.map = plan.needsMapStoreReload;
        }
    }
    const auto ownedBy = [owner](const auto& item) { return item.owner == owner; };
    tally.manifests =
        static_cast<std::size_t>(std::ranges::count_if(input.plan->Manifests(), ownedBy));
    planned -= std::min(planned, tally.manifests); // a .ymf is planned, but never registered
    tally.notRegistered = planned - std::min(planned, tally.registered);

    tally.dataFiles =
        static_cast<std::size_t>(std::ranges::count_if(input.registry->DataFiles(), ownedBy));
    const auto plannedDataFiles =
        static_cast<std::size_t>(std::ranges::count_if(input.plan->DataFiles(), ownedBy) +
                                 std::ranges::count_if(input.plan->DeferredDataFiles(), ownedBy));
    tally.dataFilesFailed = plannedDataFiles - std::min(plannedDataFiles, tally.dataFiles);

    if (const manifest::ResourceManifest* const manifest = resource.GetManifest())
    {
        tally.dataFilesRefused = static_cast<std::size_t>(std::ranges::count_if(
            manifest->dataFiles,
            [](const manifest::DataFileEntry& entry)
            {
                const streaming::DataFileTypeInfo* const info =
                    streaming::FindDataFileType(entry.type);
                return info != nullptr && info->policy == streaming::DataFilePolicy::Refused;
            }));
        tally.levelMetas = manifest->initMetas.size() + manifest->beforeLevelMetas.size() +
                           manifest->afterLevelMetas.size();
        tally.scriptEntries = manifest->ignoredScriptEntries;
    }

    for (const OverlayMount& overlay : input.overlays)
    {
        const bool crcTwin = overlay.mountPoint.find("crc:/") != std::string::npos;
        if (!crcTwin && overlay.folder.parent_path() == resource.GetRootPath() &&
            std::ranges::find(tally.mountPoints, overlay.mountPoint) == tally.mountPoints.end())
        {
            tally.mountPoints.push_back(overlay.mountPoint);
        }
    }
    return tally;
}

std::string DescribeEntry(const resource::Resource& resource, const Tally& tally)
{
    std::vector<std::string> parts;
    if (tally.registered > 0)
    {
        parts.push_back((resource.IsMod()
                             ? CountOf(tally.registered, "file streamed", "files streamed")
                             : CountOf(tally.registered, "asset", "assets")) +
                        ReplacingNote(tally.replacing));
    }
    if (tally.notRegistered > 0)
    {
        parts.push_back(fmt::format("{} not registered", tally.notRegistered));
    }
    if (tally.invalid > 0)
    {
        parts.push_back(fmt::format("{} invalid", tally.invalid));
    }
    if (tally.shadowed > 0)
    {
        parts.push_back(fmt::format("{} shadowed by another resource", tally.shadowed));
    }
    if (tally.switchedOff > 0)
    {
        parts.push_back(fmt::format("{} switched off in the configuration", tally.switchedOff));
    }
    if (!tally.mountPoints.empty())
    {
        parts.push_back("replaces game files in " + JoinMountPoints(tally.mountPoints));
    }
    if (tally.dataFiles > 0 || tally.dataFilesFailed > 0)
    {
        parts.push_back(CountOf(tally.dataFiles, "data file", "data files") +
                        (tally.dataFilesFailed > 0
                             ? fmt::format(" ({} failed)", tally.dataFilesFailed)
                             : std::string{}));
    }
    if (tally.dataFilesRefused > 0)
    {
        parts.push_back(fmt::format("{} refused as in FiveM",
                                    CountOf(tally.dataFilesRefused, "data file", "data files")));
    }
    if (tally.manifests > 0)
    {
        parts.push_back(CountOf(tally.manifests, "packfile manifest", "packfile manifests"));
    }
    if (tally.map)
    {
        parts.push_back("map");
    }
    if (tally.levelMetas > 0)
    {
        parts.push_back(CountOf(tally.levelMetas, "level meta", "level metas"));
    }
    if (tally.scriptEntries > 0)
    {
        parts.push_back(CountOf(tally.scriptEntries, "script entry", "script entries") +
                        " ignored");
    }
    if (resource.GetState() == resource::ResourceState::ManifestError)
    {
        parts.push_back(resource.GetStateReason());
    }

    if (parts.empty())
    {
        parts.emplace_back("nothing to load");
    }

    std::string text = resource.GetName() + ':';
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        text += index == 0 ? " " : ", ";
        text += parts[index];
    }
    return text;
}
} // namespace

std::string CountOf(std::size_t count, std::string_view singular, std::string_view plural)
{
    return fmt::format("{} {}", count, count == 1 ? singular : plural);
}

LoadSummary BuildLoadSummary(const LoadSummaryInput& input)
{
    struct Totals
    {
        std::size_t entries = 0;
        std::size_t loaded = 0;
        std::size_t registered = 0;
        std::size_t replacing = 0;
        std::size_t notRegistered = 0;
        std::size_t dataFiles = 0;
        std::size_t overlaid = 0;
    };
    Totals resources;
    Totals mods;
    LoadSummary summary;

    for (std::size_t index = 0; index < input.resources.size(); ++index)
    {
        const resource::Resource& resource = input.resources[index];
        Totals& totals = resource.IsMod() ? mods : resources;
        std::vector<std::string>& lines = resource.IsMod() ? summary.mods : summary.resources;
        ++totals.entries;

        if (!resource.IsEnabled())
        {
            lines.push_back(fmt::format("{}: {}", resource.GetName(),
                                        resource.GetStateReason().empty()
                                            ? std::string{"disabled"}
                                            : resource.GetStateReason()));
            continue;
        }

        const Tally tally = Count(input, index, resource);
        ++totals.loaded;
        totals.registered += tally.registered;
        totals.replacing += tally.replacing;
        totals.notRegistered += tally.notRegistered;
        totals.dataFiles += tally.dataFiles;
        totals.overlaid += tally.mountPoints.empty() ? 0 : 1;
        lines.push_back(DescribeEntry(resource, tally));
    }

    const auto notRegistered = [](const Totals& totals)
    {
        return totals.notRegistered > 0 ? fmt::format(", {} not registered", totals.notRegistered)
                                        : std::string{};
    };
    if (resources.entries > 0)
    {
        summary.resourceTotals = fmt::format(
            "Resources: {} of {} loaded, {}{}{}, {}", resources.loaded, resources.entries,
            CountOf(resources.registered, "asset", "assets"), ReplacingNote(resources.replacing),
            notRegistered(resources), CountOf(resources.dataFiles, "data file", "data files"));
    }
    if (mods.entries > 0)
    {
        summary.modTotals =
            fmt::format("Mods: {} of {} loaded, {}{}{}, {}{}", mods.loaded, mods.entries,
                        CountOf(mods.registered, "file streamed", "files streamed"),
                        ReplacingNote(mods.replacing), notRegistered(mods),
                        CountOf(mods.dataFiles, "data file", "data files"),
                        mods.overlaid > 0 ? fmt::format(", {} replacing game files in place",
                                                        CountOf(mods.overlaid, "mod", "mods"))
                                          : std::string{});
    }
    return summary;
}
} // namespace spl
