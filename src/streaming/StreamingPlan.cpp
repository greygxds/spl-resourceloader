#include "streaming/StreamingPlan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "logging/Logger.h"
#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "streaming/AssetScanner.h"
#include "streaming/AssetType.h"
#include "streaming/DataFileType.h"
#include "streaming/StreamAsset.h"
#include "util/FileTree.h"
#include "util/Glob.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
/// Disk reads stop scaling well past a handful of threads, and the game is starting up
/// alongside us.
constexpr unsigned kMaxScanThreads = 8;

/// AssetScanner::Scan for every resource in indices, in parallel. The result at position i
/// belongs to resources[indices[i]].
std::vector<AssetScanner::Result> ScanInParallel(std::span<const resource::Resource> resources,
                                                 const std::vector<std::size_t>& indices,
                                                 const AssetScanner::Options& options,
                                                 unsigned maxThreads)
{
    std::vector<AssetScanner::Result> results(indices.size());
    std::atomic<std::size_t> next{0};
    const auto work = [&]
    {
        for (std::size_t position = next++; position < indices.size(); position = next++)
        {
            const std::size_t index = indices[position];
            try
            {
                results[position] = AssetScanner::Scan(
                    resources[index], resource::ResourceId{static_cast<uint16_t>(index)}, options);
            }
            catch (const std::exception& exception)
            {
                results[position].warnings.push_back(
                    fmt::format("{}: stream/ could not be scanned: {}", resources[index].GetName(),
                                exception.what()));
            }
        }
    };

    const std::size_t threadCount = std::clamp<std::size_t>(
        std::min<std::size_t>(std::thread::hardware_concurrency(), indices.size()), 1, maxThreads);
    {
        std::vector<std::jthread> helpers;
        helpers.reserve(threadCount - 1);
        for (std::size_t helper = 1; helper < threadCount; ++helper)
        {
            helpers.emplace_back(work);
        }
        work(); // this thread takes its share instead of waiting
    }
    return results;
}

constexpr std::string_view kItypRequestType = "DLC_ITYP_REQUEST";

/// FiveM ignores every data file of a resource that ships this: an assault_vehicles pack made
/// obsolete by 1.0.1365, which crashes the game (ResourcesTest.cpp:162-170).
constexpr std::string_view kObsoleteAssaultVehiclesFile = "data/ai/vehicleweapons_caracara.meta";

/// Audio data files are named without the suffix the audio engine adds: 'audio/x_game.dat'
/// is x_game.dat151.rel on disk.
constexpr std::string_view kAudioDataSuffix = ".rel";

/// pgRawStreamer holds 65 535 entries at most, and it is shared with the game's own files.
/// Well before that the plan is a sign that something has gone wrong.
constexpr std::size_t kRawStreamerWarningCount = 50000;

/// A ResourceId is a uint16_t, which is far more resources than anyone will ever install.
constexpr std::size_t kMaxResources = std::numeric_limits<uint16_t>::max();

/// "ytd 1, ydr 2", in table order, counting only the types that occur.
std::string DescribeTypeCounts(std::span<const StreamAsset> assets)
{
    std::array<std::size_t, kAssetTypeCount> counts{};
    for (const StreamAsset& asset : assets)
    {
        ++counts[static_cast<std::size_t>(asset.type)];
    }

    std::string description;
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        const std::size_t count = counts[static_cast<std::size_t>(info.type)];
        if (count == 0)
        {
            continue;
        }
        if (!description.empty())
        {
            description += ", ";
        }
        description += fmt::format("{} {}", info.extension, count);
    }
    return description;
}

PlannedAsset ToPlannedAsset(const StreamAsset& asset)
{
    return PlannedAsset{.owner = asset.owner,
                        .resourceName = asset.resourceName,
                        .absolutePath = asset.absolutePath,
                        .relativePath = asset.relativePath,
                        .fileName = asset.fileName,
                        .streamingName = asset.streamingName,
                        .extension = asset.extension,
                        .type = asset.type,
                        .fileSizeBytes = asset.fileSizeBytes};
}

void Shadow(StreamAsset& asset, std::string reason)
{
    asset.disposition = AssetDisposition::ShadowedByDuplicate;
    asset.dispositionReason = std::move(reason);
}

struct ResolvedDataFilePaths
{
    std::filesystem::path root;             ///< empty when an @resource names no resource
    const util::IFileTree* files = nullptr; ///< non-null whenever root is set
    std::vector<std::string> relativePaths;
};

/// The files a data_file entry names. An '@other/path' entry is globbed in the other resource,
/// which FiveM allows (ResourceMetaDataComponent.cpp:408-466); a literal that matches nothing
/// is kept as written, as the manifest does for local paths.
[[nodiscard]] ResolvedDataFilePaths
ResolveDataFilePaths(const manifest::DataFileEntry& entry, const resource::Resource& resource,
                     std::span<const resource::Resource> resources)
{
    if (entry.otherResource.empty())
    {
        return ResolvedDataFilePaths{.root = resource.GetRootPath(),
                                     .files = &resource.GetFiles(),
                                     .relativePaths = entry.resolved};
    }
    const auto other = std::ranges::find_if(
        resources, [&entry](const resource::Resource& candidate)
        { return util::EqualsIgnoreCase(candidate.GetName(), entry.otherResource); });
    if (other == resources.end())
    {
        return {};
    }
    ResolvedDataFilePaths paths{
        .root = other->GetRootPath(),
        .files = &other->GetFiles(),
        .relativePaths = util::GlobFiles(other->GetFiles(), other->GetRootPath(), entry.pattern)};
    if (paths.relativePaths.empty() && util::IsLiteralPattern(entry.pattern))
    {
        paths.relativePaths.push_back(entry.pattern);
    }
    return paths;
}

[[nodiscard]] bool IsBlockedDataFile(const manifest::DataFileEntry& entry)
{
    return util::EqualsIgnoreCase(entry.pattern, kObsoleteAssaultVehiclesFile);
}

/// The [data_files] setting that switches this type off, as the text of a skip reason, or
/// std::nullopt when the type may load.
[[nodiscard]] std::optional<std::string>
FindDataFileSwitch(const DataFileTypeInfo& info, const config::DataFileSettings& settings)
{
    if (!settings.enabled)
    {
        return std::string{"data_files.enabled = false"};
    }
    if (!IsCategoryEnabled(info.category, settings))
    {
        return fmt::format("data_files.{} = false", ToString(info.category));
    }
    const bool disabled = std::ranges::any_of(settings.disabledTypes, [&](const std::string& name)
                                              { return util::EqualsIgnoreCase(name, info.name); });
    if (disabled)
    {
        return fmt::format("data_files.disabled_types lists {}", info.name);
    }
    return std::nullopt;
}

/// True when a data file's path names an existing file. An audio path names its data without
/// the engine's suffix ('x_game.dat' for x_game.dat151.rel), or a wave pack folder.
[[nodiscard]] bool HasData(const util::IFileTree& files, const std::filesystem::path& path,
                           DataFileCategory category)
{
    if (files.Stat(path))
    {
        return true;
    }
    if (category != DataFileCategory::Audio)
    {
        return false;
    }

    const std::string prefix = util::ToLower(util::ToUtf8(path.filename()));
    const Result<std::vector<util::FileTreeEntry>> entries = files.List(path.parent_path());
    if (!entries)
    {
        return false;
    }
    for (const util::FileTreeEntry& entry : entries.GetValue())
    {
        const std::string name = util::ToLower(util::ToUtf8(entry.path.filename()));
        if (name.starts_with(prefix) && name.ends_with(kAudioDataSuffix))
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool IsSupportedDataFileType(std::string_view type)
{
    const DataFileTypeInfo* const info = FindDataFileType(type);
    return info != nullptr && info->policy != DataFilePolicy::Refused;
}

StreamingPlan StreamingPlan::Build(std::span<resource::Resource> resources,
                                   const config::StreamingSettings& streaming,
                                   const config::DiagnosticsSettings& diagnostics,
                                   const config::DataFileSettings& dataFiles)
{
    StreamingPlan plan;

    if (!streaming.enabled)
    {
        SPL_LOG_INFO(Streaming, "Streaming is disabled by configuration; nothing will be loaded");
        return plan;
    }

    const AssetScanner::Options options{.validateRscHeaders = diagnostics.validateRscHeaders,
                                        .assetSizeWarningMiB = diagnostics.assetSizeWarningMiB};

    std::vector<std::size_t> scanned;
    for (std::size_t index = 0; index < resources.size(); ++index)
    {
        if (!resources[index].IsEnabled())
        {
            continue;
        }
        if (index > kMaxResources)
        {
            SPL_LOG_ERROR(Streaming, "More than {} resources; the rest are ignored", kMaxResources);
            break;
        }
        scanned.push_back(index);
    }

    // The scans are the slow part of startup: every asset's header is read from disk. They
    // are pure and independent, so they run in parallel; everything that logs or orders runs
    // afterwards, on this thread, in load order.
    std::vector<AssetScanner::Result> scans =
        ScanInParallel(resources, scanned, options, kMaxScanThreads);
    for (std::size_t position = 0; position < scanned.size(); ++position)
    {
        resource::Resource& resource = resources[scanned[position]];
        const resource::ResourceId owner{static_cast<uint16_t>(scanned[position])};
        plan.AddResourceAssets(resource, owner, std::move(scans[position]));
        resource.SetState(resource::ResourceState::Scanned);
    }

    plan.ApplyConfigGates(streaming);
    plan.ResolveDuplicates(streaming);
    plan.CollectDataFiles(resources, dataFiles);
    if (streaming.autoRequestYtyp)
    {
        plan.AddImplicitTypeRequests();
    }
    plan.Order();
    return plan;
}

void StreamingPlan::AddResourceAssets(const resource::Resource& resource,
                                      resource::ResourceId owner, AssetScanner::Result scan)
{
    const manifest::ResourceManifest* manifest = resource.GetManifest();

    ResourcePlan& resourcePlan = m_resources.emplace_back(
        ResourcePlan{.owner = owner,
                     .name = resource.GetName(),
                     .isMod = resource.IsMod(),
                     // A resource that calls itself a map needs the map store reloaded even
                     // when it ships no ymap of its own (FiveM: LevelLoader.cpp:488-507).
                     .needsMapStoreReload = manifest != nullptr && manifest->isMap});

    for (const std::string& warning : scan.warnings)
    {
        SPL_LOG_WARNING(Streaming, warning);
    }
    for (const std::string& note : scan.notes)
    {
        SPL_LOG_DEBUG(Streaming, note);
    }

    if (scan.assets.empty())
    {
        SPL_LOG_DEBUG(Streaming, "{}: no streaming assets", resourcePlan.name);
        return;
    }

    SPL_LOG_DEBUG(Streaming, "{}: found {} streaming assets ({})", resourcePlan.name,
                  scan.assets.size(), DescribeTypeCounts(scan.assets));

    m_assets.insert(m_assets.end(), std::make_move_iterator(scan.assets.begin()),
                    std::make_move_iterator(scan.assets.end()));
}

void StreamingPlan::ApplyConfigGates(const config::StreamingSettings& streaming)
{
    for (StreamAsset& asset : m_assets)
    {
        if (asset.disposition != AssetDisposition::Planned)
        {
            continue;
        }

        const AssetTypeInfo& info = GetAssetTypeInfo(asset.type);
        if (IsGateOpen(info.gate, streaming))
        {
            continue;
        }

        asset.disposition = AssetDisposition::SkippedByConfig;
        asset.dispositionReason = fmt::format("{} = false", ToString(info.gate));
        SPL_LOG_DEBUG(Streaming, "{}: skipping '{}' ({})", asset.resourceName, asset.relativePath,
                      asset.dispositionReason);
    }
}

bool StreamingPlan::IsMod(resource::ResourceId owner) const
{
    const auto found = std::ranges::find(m_resources, owner, &ResourcePlan::owner);
    return found != m_resources.end() && found->isMod;
}

void StreamingPlan::ResolveDuplicates(const config::StreamingSettings& streaming)
{
    // File name to the index of the asset that currently owns it. Assets are visited in load
    // order, so "first" and "last" mean what the user's priority list says they mean.
    std::unordered_map<std::string, std::size_t> winners;

    for (std::size_t index = 0; index < m_assets.size(); ++index)
    {
        StreamAsset& asset = m_assets[index];
        // A .ymf never takes a streaming slot: the game files its entries under the resource
        // that ships it, so every map resource has its own _manifest.ymf and none of them
        // compete for the name.
        if (asset.disposition != AssetDisposition::Planned ||
            asset.type == AssetType::PackfileManifest)
        {
            continue;
        }

        const auto existing = winners.find(asset.fileName);
        if (existing == winners.end())
        {
            winners.emplace(asset.fileName, index);
            continue;
        }

        StreamAsset& incumbent = m_assets[existing->second];

        // Two files of the same name inside one resource: the game would keep whichever was
        // registered last, which depends on enumeration order. We keep the first sorted path
        // instead, so the outcome is the same on every machine.
        if (incumbent.owner == asset.owner)
        {
            Shadow(asset, fmt::format("'{}' also provides this file name", incumbent.relativePath));
            SPL_LOG_WARNING(
                Streaming, "{}: '{}' is provided twice ('{}' and '{}') — using the first",
                asset.resourceName, asset.fileName, incumbent.relativePath, asset.relativePath);
            continue;
        }

        const bool lastWins = streaming.duplicatePolicy == config::DuplicatePolicy::LastWins;
        const std::string_view policy = lastWins ? "last" : "first";
        const std::string& usedResource = lastWins ? asset.resourceName : incumbent.resourceName;

        // Mods load after every resource by design, so a resource beating a mod is expected;
        // the summary still counts the shadowed file.
        const bool resourceBeatsMod = IsMod(incumbent.owner) != IsMod(asset.owner);
        logging::Get(logging::Channel::Streaming)
            ->log(resourceBeatsMod ? spdlog::level::debug : spdlog::level::warn,
                  "'{}' is provided by both '{}' and '{}' — using '{}' (duplicate_policy = "
                  "{})",
                  asset.fileName, incumbent.resourceName, asset.resourceName, usedResource, policy);

        if (lastWins)
        {
            Shadow(incumbent, fmt::format("'{}' provides the same file name", asset.resourceName));
            existing->second = index;
        }
        else
        {
            Shadow(asset, fmt::format("'{}' provides the same file name", incumbent.resourceName));
        }
    }
}

void StreamingPlan::CollectDataFiles(std::span<const resource::Resource> resources,
                                     const config::DataFileSettings& settings)
{
    // Streaming name to the streamed .ytyp that provides it. A planned copy beats a skipped one,
    // so a shadowed duplicate never hides the copy that will actually be registered.
    std::unordered_map<std::string, const StreamAsset*> streamedTypes;
    for (const StreamAsset& asset : m_assets)
    {
        if (asset.type != AssetType::MapTypes)
        {
            continue;
        }
        const auto [existing, inserted] = streamedTypes.try_emplace(asset.streamingName, &asset);
        if (!inserted && asset.disposition == AssetDisposition::Planned)
        {
            existing->second = &asset;
        }
    }

    for (const ResourcePlan& resourcePlan : m_resources)
    {
        const resource::Resource& resource = resources[resourcePlan.owner.value];
        const manifest::ResourceManifest* manifest = resource.GetManifest();
        if (manifest == nullptr)
        {
            continue;
        }

        if (const auto blocked = std::ranges::find_if(manifest->dataFiles, IsBlockedDataFile);
            blocked != manifest->dataFiles.end())
        {
            SPL_LOG_WARNING(Streaming,
                            "{}: '{}' belongs to an obsolete assault_vehicles pack that crashes "
                            "the game; none of this resource's data files are loaded",
                            resourcePlan.name, blocked->pattern);
            continue;
        }

        for (const manifest::DataFileEntry& entry : manifest->dataFiles)
        {
            const std::string type = util::ToUpper(entry.type);
            const DataFileTypeInfo* const info = FindDataFileType(type);
            if (info == nullptr)
            {
                SPL_LOG_WARNING(Streaming,
                                "{}: data_file type '{}' does not exist in GTA V — '{}' is ignored",
                                resourcePlan.name, entry.type, entry.pattern);
                continue;
            }
            if (info->policy == DataFilePolicy::Refused)
            {
                SPL_LOG_DEBUG(Streaming,
                              "{}: data_file type '{}' is refused, as FiveM refuses it (the game "
                              "cannot load it at runtime) — '{}' is ignored",
                              resourcePlan.name, type, entry.pattern);
                continue;
            }
            if (const std::optional<std::string> reason = FindDataFileSwitch(*info, settings))
            {
                SPL_LOG_DEBUG(Streaming, "{}: skipping data_file {} '{}' ({})", resourcePlan.name,
                              type, entry.pattern, *reason);
                continue;
            }

            const ResolvedDataFilePaths paths = ResolveDataFilePaths(entry, resource, resources);
            if (paths.root.empty())
            {
                SPL_LOG_WARNING(Streaming,
                                "{}: data_file '@{}/{}' names a resource that does not exist",
                                resourcePlan.name, entry.otherResource, entry.pattern);
                continue;
            }
            if (paths.relativePaths.empty())
            {
                SPL_LOG_WARNING(Streaming, "{}: data_file '{}' matches no file", resourcePlan.name,
                                entry.pattern);
                continue;
            }

            for (const std::string& relativePath : paths.relativePaths)
            {
                const std::filesystem::path path{relativePath};
                PlannedDataFile dataFile{
                    .owner = resourcePlan.owner,
                    .resourceName = resourcePlan.name,
                    .type = type,
                    .policy = info->policy,
                    .loadOrder = info->loadOrder,
                    .absolutePath = paths.root / relativePath,
                    .relativePath = entry.otherResource.empty()
                                        ? relativePath
                                        : fmt::format("@{}/{}", entry.otherResource, relativePath),
                    .fileName = util::ToLower(util::ToUtf8(path.filename()))};

                if (info->policy != DataFilePolicy::TypeRequest &&
                    !HasData(*paths.files, dataFile.absolutePath, info->category))
                {
                    // Still handed over, as FiveM does: the game's mounter has the last word.
                    SPL_LOG_WARNING(Streaming,
                                    "{}: data_file {} '{}' does not exist; the game will most "
                                    "likely refuse it",
                                    resourcePlan.name, type, relativePath);
                }

                if (info->policy == DataFilePolicy::TypeRequest)
                {
                    const auto match = streamedTypes.find(util::ToLower(util::ToUtf8(path.stem())));
                    if (match == streamedTypes.end())
                    {
                        // Still loaded: it may name a .ytyp that ships with the game or a DLC.
                        SPL_LOG_WARNING(Streaming,
                                        "{}: DLC_ITYP_REQUEST '{}' does not match any streamed "
                                        ".ytyp",
                                        resourcePlan.name, dataFile.fileName);
                    }
                    else if (const StreamAsset& streamed = *match->second;
                             streamed.disposition != AssetDisposition::Planned)
                    {
                        SPL_LOG_DEBUG(Streaming,
                                      "{}: DLC_ITYP_REQUEST '{}' is not loaded because its .ytyp "
                                      "is {} ({})",
                                      resourcePlan.name, dataFile.fileName,
                                      ToString(streamed.disposition), streamed.dispositionReason);
                        continue;
                    }
                    else
                    {
                        dataFile.matchesStreamedAsset = true;
                        dataFile.fileName = streamed.fileName;
                    }
                }

                // FiveM mounts these once the map store has been rebuilt
                // (LoadStreamingFile.cpp:1206).
                std::vector<PlannedDataFile>& list =
                    info->policy == DataFilePolicy::Deferred ? m_deferredDataFiles : m_dataFiles;
                list.push_back(std::move(dataFile));
            }
        }
    }
}

void StreamingPlan::AddImplicitTypeRequests()
{
    for (const StreamAsset& asset : m_assets)
    {
        if (asset.type != AssetType::MapTypes || asset.disposition != AssetDisposition::Planned)
        {
            continue;
        }

        const bool requested =
            std::ranges::any_of(m_dataFiles,
                                [&asset](const PlannedDataFile& dataFile)
                                {
                                    return dataFile.policy == DataFilePolicy::TypeRequest &&
                                           dataFile.fileName == asset.fileName;
                                });
        // A resource with a .ymf decides through it which types its maps pull in.
        const bool hasPackfileManifest =
            std::ranges::any_of(m_assets,
                                [&asset](const StreamAsset& other)
                                {
                                    return other.owner == asset.owner &&
                                           other.type == AssetType::PackfileManifest &&
                                           other.disposition == AssetDisposition::Planned;
                                });
        if (requested || hasPackfileManifest)
        {
            continue;
        }

        SPL_LOG_DEBUG(Streaming,
                      "{}: requesting '{}' without a DLC_ITYP_REQUEST (auto_request_ytyp)",
                      asset.resourceName, asset.fileName);
        m_dataFiles.push_back(PlannedDataFile{.owner = asset.owner,
                                              .resourceName = asset.resourceName,
                                              .type = std::string{kItypRequestType},
                                              .policy = DataFilePolicy::TypeRequest,
                                              .absolutePath = asset.absolutePath,
                                              .relativePath = asset.relativePath,
                                              .fileName = asset.fileName,
                                              .matchesStreamedAsset = true,
                                              .implicit = true});
    }
}

void StreamingPlan::Order()
{
    const auto planFor = [this](resource::ResourceId owner) -> ResourcePlan*
    {
        const auto match = std::ranges::find_if(m_resources, [owner](const ResourcePlan& candidate)
                                                { return candidate.owner == owner; });
        return match != m_resources.end() ? &*match : nullptr;
    };

    for (const StreamAsset& asset : m_assets)
    {
        ResourcePlan* resourcePlan = planFor(asset.owner);
        if (asset.disposition != AssetDisposition::Planned)
        {
            if (resourcePlan != nullptr)
            {
                ++resourcePlan->skippedAssets;
            }
            continue;
        }

        if (resourcePlan != nullptr)
        {
            ++resourcePlan->plannedAssets;
        }

        if (asset.type == AssetType::PackfileManifest)
        {
            m_manifests.push_back(PlannedManifest{.owner = asset.owner,
                                                  .resourceName = asset.resourceName,
                                                  .absolutePath = asset.absolutePath,
                                                  .relativePath = asset.relativePath,
                                                  .fileName = asset.fileName});
            if (resourcePlan != nullptr)
            {
                resourcePlan->hasPackfileManifest = true;
            }
            continue;
        }

        if (asset.type == AssetType::MapData || asset.type == AssetType::StaticBounds)
        {
            if (resourcePlan != nullptr)
            {
                resourcePlan->needsMapStoreReload = true;
            }
        }

        const AssetTypeInfo& info = GetAssetTypeInfo(asset.type);
        std::vector<PlannedAsset>& stage =
            info.stage == RegistrationStage::Early ? m_early : m_late;
        stage.push_back(ToPlannedAsset(asset));
    }

    // Stable, so load order still decides between two assets of the same type.
    const auto byRegistrationOrder = [](const PlannedAsset& left, const PlannedAsset& right)
    {
        return GetAssetTypeInfo(left.type).registrationOrder <
               GetAssetTypeInfo(right.type).registrationOrder;
    };
    std::ranges::stable_sort(m_early, byRegistrationOrder);
    std::ranges::stable_sort(m_late, byRegistrationOrder);

    // FiveM's dfSort: handling and vehicle layouts before everything that refers to them, the
    // rest in manifest order.
    std::ranges::stable_sort(m_dataFiles, std::less{}, &PlannedDataFile::loadOrder);
    std::ranges::stable_sort(m_deferredDataFiles, std::less{}, &PlannedDataFile::loadOrder);

    const std::size_t registrations = m_early.size() + m_late.size();
    if (registrations > kRawStreamerWarningCount)
    {
        SPL_LOG_WARNING(Streaming,
                        "{} assets is close to the game's limit of 65535 raw streaming entries; "
                        "expect trouble",
                        registrations);
    }
}

std::size_t
StreamingPlan::RemoveDataFilesIf(const std::function<bool(const PlannedDataFile&)>& predicate)
{
    return std::erase_if(m_dataFiles, predicate) + std::erase_if(m_deferredDataFiles, predicate);
}

bool StreamingPlan::NeedsMapStoreReload() const
{
    return std::ranges::any_of(m_resources, [](const ResourcePlan& resourcePlan)
                               { return resourcePlan.needsMapStoreReload; });
}

std::size_t StreamingPlan::CountOf(AssetType type) const
{
    const auto ofType = [type](const PlannedAsset& asset) { return asset.type == type; };
    return static_cast<std::size_t>(std::ranges::count_if(m_early, ofType)) +
           static_cast<std::size_t>(std::ranges::count_if(m_late, ofType)) +
           (type == AssetType::PackfileManifest ? m_manifests.size() : 0);
}

std::size_t StreamingPlan::CountOf(AssetDisposition disposition) const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(m_assets, [disposition](const StreamAsset& asset)
                              { return asset.disposition == disposition; }));
}

void StreamingPlan::LogSummary() const
{
    const std::size_t skipped = m_assets.size() - CountOf(AssetDisposition::Planned);

    SPL_LOG_DEBUG(Streaming,
                  "Streaming plan: {} early, {} late, {} manifest(s), {} data file(s), {} "
                  "skipped{}",
                  m_early.size(), m_late.size(), m_manifests.size(),
                  m_dataFiles.size() + m_deferredDataFiles.size(), skipped,
                  NeedsMapStoreReload() ? "; map store reload required" : "");

    // Every planned file becomes a raw streamer entry. The game's raw streamer is sized for its
    // own loose files, and nobody has measured where it gives out.
    const std::size_t rawEntries = m_early.size() + m_late.size() + m_manifests.size();
    if (rawEntries > kRawStreamerWarningThreshold)
    {
        SPL_LOG_WARNING(Streaming,
                        "The plan registers {} files with the game's raw streamer, more than the "
                        "{} this loader has been tested with; expect long load times or failed "
                        "registrations",
                        rawEntries, kRawStreamerWarningThreshold);
    }

    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        const std::size_t count = CountOf(info.type);
        if (count > 0)
        {
            SPL_LOG_DEBUG(Streaming, "Planned .{}: {}", info.extension, count);
        }
    }

    for (const StreamAsset& asset : m_assets)
    {
        if (asset.disposition != AssetDisposition::Planned)
        {
            SPL_LOG_DEBUG(Streaming, "{}: '{}' {} ({})", asset.resourceName, asset.relativePath,
                          ToString(asset.disposition), asset.dispositionReason);
        }
    }
}
} // namespace spl::streaming
