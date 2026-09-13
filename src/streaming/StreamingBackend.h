#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/Result.h"
#include "rage/types/StreamingTypes.h"
#include "streaming/AssetRegistry.h"
#include "streaming/StreamingPlan.h"

namespace spl::streaming
{
/// What became of one registration attempt. Skipped and KeptGameAsset are not failures: the
/// asset was deliberately left alone, and the message says why.
enum class RegistrationStatus
{
    Registered,
    Skipped,
    KeptGameAsset, ///< the game already has the name and streaming.allow_overrides is false
    Failed
};

/// When a registered override becomes what the game uses.
enum class OverrideTiming
{
    NextLoad,   ///< the game's copy was not loaded, or was released: the next request loads ours
    AfterUnload ///< the game's copy is in use; ours loads once the game lets go of it
};

struct RegistrationOutcome
{
    RegistrationStatus status = RegistrationStatus::Failed;
    std::string vfsPath;           ///< empty unless the asset reached the game
    rage::GlobalIndex globalIndex; ///< only meaningful when Registered
    rage::StreamingHandle handle;  ///< only meaningful when Registered

    /// The game's own handle, when the asset took over a slot that already had one.
    std::optional<rage::StreamingHandle> replacedHandle;
    OverrideTiming overrideTiming = OverrideTiming::NextLoad; ///< only with replacedHandle

    std::string message; ///< ready to log; empty when Registered
};

/// What loading one packfile manifest changed.
struct PackfileManifestOutcome
{
    /// Entry names of DLC_ITYP_REQUEST data files the manifest took over and the game released.
    std::vector<std::string> releasedTypeRequests;
    std::vector<MapDependency> mapDependencies;
};

/// What the game has to be told once the regular data files are in.
struct DataFileFinishRequest
{
    /// A CARCOLS_FILE loaded, so the vehicle paint ramps need building again (build 2545+).
    bool vehicleColoursLoaded = false;

    /// "myped" for components streamed as "myped^head_000_r.ydd": the ped model that has to be
    /// told its stream folder (FiveM LoadStreamingFile.cpp:2397).
    std::vector<std::string> pedFolders;
};

/// What putting a displaced registration back changed.
struct ReassertOutcome
{
    rage::StreamingHandle displaced; ///< the game's handle that had taken the slot
    OverrideTiming timing = OverrideTiming::NextLoad;
};

struct MapReloadReport
{
    std::string method; ///< "change set replay", for the log
    std::size_t collisionsPreloaded = 0;
    int64_t preloadMs = 0;
    int64_t totalMs = 0;
};

/// Everything the streaming state machine needs from the game, behind one interface so the
/// machine itself stays pure and can be tested against a fake.
class IStreamingBackend
{
public:
    IStreamingBackend() = default;
    IStreamingBackend(const IStreamingBackend&) = delete;
    IStreamingBackend& operator=(const IStreamingBackend&) = delete;
    virtual ~IStreamingBackend() = default;

    /// Makes root reachable by the game, so that a VFS path can name a file inside it.
    [[nodiscard]] virtual Result<void> PrepareResourceRoot(const std::filesystem::path& root) = 0;

    /// Makes one asset known to the game. Never throws and never reports absence as failure.
    [[nodiscard]] virtual RegistrationOutcome RegisterAsset(const PlannedAsset& asset) = 0;

    /// Applies the game patches raw .ytyp and .ymap files need. Called once, before the late
    /// registrations, and only when the plan has such files.
    [[nodiscard]] virtual Result<void> InstallMapTypesPatches() = 0;

    /// Applies the game patches raw .ymap and .ybn files need. Called once, before the late
    /// registrations, and only when the plan has such files.
    [[nodiscard]] virtual Result<void> InstallMapDataPatches() = 0;

    /// Hands one data file to the game's mounter. Returns the path the game was given.
    [[nodiscard]] virtual Result<std::string> LoadDataFile(const PlannedDataFile& dataFile) = 0;

    /// Runs after the data-file stage, only when request has something in it. The backend logs
    /// what it could not do; nothing here stops the pump.
    virtual void FinishDataFiles(const DataFileFinishRequest& request)
    {
        (void)request;
    }

    /// Feeds one .ymf to the game. A .ytyp it names that is loaded through one of
    /// loadedDataFiles is handed over to the map store (FiveM LoadStreamingFile.cpp:2292).
    [[nodiscard]] virtual Result<PackfileManifestOutcome>
    LoadPackfileManifest(const PlannedManifest& manifest,
                         std::span<const LoadedDataFile> loadedDataFiles) = 0;

    /// False while a rebuild would run under a loading screen or a character switch; the
    /// reload then waits for a later tick.
    [[nodiscard]] virtual bool CanReloadMapStore() = 0;

    /// Preloads the collisions and rebuilds the map data store. Blocks the calling tick.
    [[nodiscard]] virtual Result<MapReloadReport>
    ReloadMapStore(std::span<const RegisteredAsset> collisions) = 0;

    /// Puts asset's handle back when the game has registered its own file in the slot since, as
    /// its DLC change sets do while a session starts. std::nullopt when the slot still carries
    /// ours, or when it cannot be checked. Defaults to that for test fakes.
    [[nodiscard]] virtual std::optional<ReassertOutcome> ReassertAsset(const RegisteredAsset& asset)
    {
        (void)asset;
        return std::nullopt;
    }

    [[nodiscard]] virtual Result<void> UnregisterAsset(const RegisteredAsset& asset) = 0;
};
} // namespace spl::streaming
