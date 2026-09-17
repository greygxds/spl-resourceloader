#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/LoaderConfig.h"
#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "streaming/AssetScanner.h"
#include "streaming/AssetType.h"
#include "streaming/DataFileType.h"
#include "streaming/StreamAsset.h"

namespace spl::streaming
{
/// An asset the loader intends to register, reduced to what the registration code needs.
struct PlannedAsset
{
    resource::ResourceId owner;
    std::string resourceName;
    std::filesystem::path absolutePath;
    std::string relativePath;
    std::string fileName;      ///< "prop_a.ydr", the name the game will know it by
    std::string streamingName; ///< "prop_a", the name a streaming module looks up
    std::string extension;     ///< "ydr"; for OtherModule, the store to look up
    AssetType type = AssetType::Unknown;
    uint64_t fileSizeBytes = 0;
};

/// A .ymf packfile manifest. It is never a streaming object: it goes to the game's manifest
/// chunk loader instead.
struct PlannedManifest
{
    resource::ResourceId owner;
    std::string resourceName;
    std::filesystem::path absolutePath;
    std::string relativePath;
    std::string fileName;
};

/// One file behind a supported `data_file` entry of a manifest.
struct PlannedDataFile
{
    resource::ResourceId owner;
    std::string resourceName;
    std::string type; ///< upper-cased, e.g. "DLC_ITYP_REQUEST"
    DataFilePolicy policy = DataFilePolicy::Generic;
    DataFileLoadOrder loadOrder = DataFileLoadOrder::Normal;
    std::filesystem::path absolutePath;
    std::string relativePath;
    std::string fileName; ///< lower-case; the streamed asset's own file name when it matches one

    /// A planned asset of some resource provides this file, so it has to be registered before
    /// the data file is loaded. False means it names a game file, or nothing at all.
    bool matchesStreamedAsset = false;

    /// What the resource ships under this name, when the game refusing the file would otherwise
    /// say nothing about why ("it ships x_game.dat151 without its .nametable"). Empty when the
    /// path names a file outright.
    std::string contentNote;

    /// Added by [streaming] auto_request_ytyp rather than written in the manifest.
    bool implicit = false;
};

/// What the plan holds for one resource, for the summary and for registration's decisions.
struct ResourcePlan
{
    resource::ResourceId owner;
    std::string name;
    uint32_t plannedAssets = 0;
    uint32_t skippedAssets = 0;
    bool isMod = false; ///< a user mod, which loses same-named files to resources

    /// The map store has to be reloaded before the game sees this resource's maps
    /// (FiveM: LevelLoader.cpp:488-507).
    bool needsMapStoreReload = false;

    /// The resource ships a .ymf, so there is a packfile manifest to load.
    bool hasPackfileManifest = false;
};

/// The ordered list of what gets registered, built once from every enabled resource.
///
/// Pure code: it reads the filesystem and logs, and makes no game calls. Build() is where
/// configuration gates, duplicate policy and registration order are applied, so the code
/// that talks to the game has nothing left to decide.
class StreamingPlan
{
public:
    /// Above this many raw streamer entries the summary warns.
    static constexpr std::size_t kRawStreamerWarningThreshold = 50000;

    /// Scans every enabled resource, resolves conflicts and orders the result. Marks the
    /// resources it scanned as Scanned, which is why the span is mutable.
    [[nodiscard]] static StreamingPlan Build(std::span<resource::Resource> resources,
                                             const config::StreamingSettings& streaming,
                                             const config::DiagnosticsSettings& diagnostics,
                                             const config::DataFileSettings& dataFiles = {});

    /// Textures and models: registered as soon as the bridge is up.
    [[nodiscard]] std::span<const PlannedAsset> Early() const
    {
        return m_early;
    }

    /// Types, collisions and maps, in that order: they have to wait until the session exists.
    [[nodiscard]] std::span<const PlannedAsset> Late() const
    {
        return m_late;
    }

    [[nodiscard]] std::span<const PlannedManifest> Manifests() const
    {
        return m_manifests;
    }

    [[nodiscard]] std::span<const PlannedDataFile> DataFiles() const
    {
        return m_dataFiles;
    }

    /// GTXD_PARENTING_DATA: loaded after the map store reload, whether one was needed or not.
    [[nodiscard]] std::span<const PlannedDataFile> DeferredDataFiles() const
    {
        return m_deferredDataFiles;
    }

    /// Drops the data files that match, and returns how many. For files the game turns out to
    /// read on its own, which loading again would load twice.
    std::size_t RemoveDataFilesIf(const std::function<bool(const PlannedDataFile&)>& predicate);

    /// Every discovered asset, skipped ones included, in scan order. The diagnostic view.
    [[nodiscard]] std::span<const StreamAsset> Assets() const
    {
        return m_assets;
    }

    [[nodiscard]] std::span<const ResourcePlan> Resources() const
    {
        return m_resources;
    }

    /// True when any resource needs the map store reloaded.
    [[nodiscard]] bool NeedsMapStoreReload() const;

    /// How many planned assets have this type, manifests included.
    [[nodiscard]] std::size_t CountOf(AssetType type) const;

    /// How many discovered assets ended up with this disposition.
    [[nodiscard]] std::size_t CountOf(AssetDisposition disposition) const;

    /// One info line with the totals, plus a debug line per type. Called by Application after
    /// Build(), so a caller that only wants the data pays nothing.
    void LogSummary() const;

private:
    /// Takes one resource's scan into the plan and logs what it found.
    void AddResourceAssets(const resource::Resource& resource, resource::ResourceId owner,
                           AssetScanner::Result scan);
    void ApplyConfigGates(const config::StreamingSettings& streaming);
    void ResolveDuplicates(const config::StreamingSettings& streaming);
    [[nodiscard]] bool IsMod(resource::ResourceId owner) const;
    void CollectDataFiles(std::span<const resource::Resource> resources,
                          const config::DataFileSettings& settings);
    void AddImplicitTypeRequests();
    void Order();

    std::vector<StreamAsset> m_assets;
    std::vector<PlannedAsset> m_early;
    std::vector<PlannedAsset> m_late;
    std::vector<PlannedManifest> m_manifests;
    std::vector<PlannedDataFile> m_dataFiles;
    std::vector<PlannedDataFile> m_deferredDataFiles;
    std::vector<ResourcePlan> m_resources;
};

/// True for a `data_file` type the loader hands to the game: any type the game knows, except the
/// ones FiveM refuses.
[[nodiscard]] bool IsSupportedDataFileType(std::string_view type);
} // namespace spl::streaming
