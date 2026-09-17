#include "streaming/StreamingManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "core/Result.h"
#include "logging/Logger.h"
#include "streaming/AssetRegistry.h"
#include "streaming/AssetType.h"
#include "streaming/StreamingBackend.h"
#include "streaming/StreamingPlan.h"
#include "util/Glob.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
/// The collision LOD prefixes a .ybn can carry ("hi@bh1_05_0.ybn"). The MP layer keeps them in
/// front of its own prefix ("hi@hei_bh1_05_0.ybn").
constexpr std::array<std::string_view, 2> kCollisionLodPrefixes = {"hi@", "ma@"};

[[nodiscard]] std::string_view WithoutCollisionLodPrefix(std::string_view fileName)
{
    for (std::string_view prefix : kCollisionLodPrefixes)
    {
        if (fileName.size() > prefix.size() &&
            util::EqualsIgnoreCase(fileName.substr(0, prefix.size()), prefix))
        {
            return fileName.substr(prefix.size());
        }
    }
    return fileName;
}

/// "ytd 3, ydr 1", in asset-type table order, counting only the types that occur.
std::string DescribeTypeCounts(const AssetRegistry& registry)
{
    std::string description;
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        const std::size_t count = registry.CountOf(info.type);
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

RegisteredAsset ToRegisteredAsset(const PlannedAsset& asset, const RegistrationOutcome& outcome)
{
    return RegisteredAsset{.owner = asset.owner,
                           .resourceName = asset.resourceName,
                           .vfsPath = outcome.vfsPath,
                           .fileName = asset.fileName,
                           .moduleExtension =
                               std::string{GetModuleExtension(asset.type, asset.extension)},
                           .type = asset.type,
                           .globalIndex = outcome.globalIndex,
                           .handle = outcome.handle,
                           .overridesGameAsset = outcome.replacedHandle.has_value()};
}

/// True when a late asset of one of these types will actually be registered.
bool HasRegistrationOf(std::span<const PlannedAsset> lateAssets,
                       std::initializer_list<AssetType> types)
{
    return std::ranges::any_of(lateAssets,
                               [types](const PlannedAsset& asset)
                               {
                                   return std::ranges::find(types, asset.type) != types.end() &&
                                          IsRegistrationImplemented(asset.type);
                               });
}
} // namespace

std::string_view ToString(StreamingStage stage)
{
    using enum StreamingStage;
    switch (stage)
    {
    case Idle:
        return "idle";
    case Mount:
        return "mount";
    case RegisterEarly:
        return "early registration";
    case GamePatches:
        return "game patches";
    case RegisterLate:
        return "late registration";
    case DataFiles:
        return "data files";
    case Manifests:
        return "packfile manifests";
    case MapReload:
        return "map store reload";
    case DeferredDataFiles:
        return "deferred data files";
    case Done:
        return "done";
    case Failed:
        return "failed";
    }
    return "idle";
}

void StreamingManager::Start(const StreamingPlan& plan, IStreamingBackend& backend,
                             std::filesystem::path resourcesRoot, Options options)
{
    m_plan = &plan;
    m_backend = &backend;
    m_resourcesRoot = std::move(resourcesRoot);
    m_options = options;
    m_options.maxRegistrationsPerTick = std::max<std::size_t>(1, options.maxRegistrationsPerTick);
    m_options.maxDataFilesPerTick = std::max<std::size_t>(1, options.maxDataFilesPerTick);
    m_options.maxManifestsPerTick = std::max<std::size_t>(1, options.maxManifestsPerTick);

    m_cursor = 0;
    m_stageTotals = {};
    m_totals = {};
    m_dataFileTotals = {};
    m_manifestTotals = {};
    m_mapReloadResult = MapReloadResult::NotNeeded;
    m_mapReloadWaitLogged = false;
    m_busy = false;
    m_work = {};
    m_timings.clear();
    m_waitingAssets.clear();
    m_registry.Clear();
    m_stage = StreamingStage::Mount;
}

int64_t StreamingManager::NowMicros() const
{
    if (m_options.clockMicros)
    {
        return m_options.clockMicros();
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool StreamingManager::IsOverBudget(std::size_t doneThisTick) const
{
    return doneThisTick > 0 && NowMicros() - m_tickStartMicros >= m_options.tickBudgetMicros;
}

void StreamingManager::SetBusy(bool busy)
{
    if (m_busy == busy)
    {
        return;
    }
    m_busy = busy;
    if (m_options.onBusyChanged)
    {
        m_options.onBusyChanged(busy);
    }
}

void StreamingManager::BeginWork(std::string_view resourceName, std::string_view fileName,
                                 std::string_view action)
{
    m_work = StreamingWork{.stage = m_stage, .resourceName = resourceName, .fileName = fileName};
    logging::Breadcrumb(fmt::format("{} '{}' from '{}'", action, fileName, resourceName));
}

void StreamingManager::Enter(StreamingStage stage)
{
    m_stage = stage;
    m_cursor = 0;
    m_stageTotals = {};
    m_work = StreamingWork{.stage = stage};
}

void StreamingManager::Tick()
{
    if (m_backend == nullptr || m_plan == nullptr || IsFinished())
    {
        return;
    }

    const StreamingStage stage = m_stage;
    m_tickStartMicros = NowMicros();
    if (stage != StreamingStage::MapReload)
    {
        SetBusy(true); // the reload decides for itself, because it may only be waiting
    }

    switch (stage)
    {
        using enum StreamingStage;
    case Mount:
        RunMount();
        break;
    case RegisterEarly:
        RunRegistrations(m_plan->Early(), GamePatches);
        break;
    case GamePatches:
        RunGamePatches();
        break;
    case RegisterLate:
        RunRegistrations(m_plan->Late(), DataFiles);
        break;
    case DataFiles:
        RunDataFiles(m_plan->DataFiles(), Manifests);
        break;
    case Manifests:
        RunManifests();
        break;
    case MapReload:
        RunMapReload();
        break;
    case DeferredDataFiles:
        RunDataFiles(m_plan->DeferredDataFiles(), Done);
        break;
    case Idle:
    case Done:
    case Failed:
        break;
    }

    const int64_t elapsed = NowMicros() - m_tickStartMicros;
    if (m_timings.empty() || m_timings.back().stage != stage)
    {
        m_timings.push_back(StageTiming{.stage = stage});
    }
    StageTiming& timing = m_timings.back();
    timing.activeMicros += elapsed;
    ++timing.ticks;
    timing.slowestTickMicros = std::max(timing.slowestTickMicros, elapsed);

    if (IsFinished())
    {
        SetBusy(false);
    }
}

void StreamingManager::RunMount()
{
    logging::Breadcrumb("mounting the resources folder");
    if (const Result<void> prepared = m_backend->PrepareResourceRoot(m_resourcesRoot); !prepared)
    {
        m_stage = StreamingStage::Failed;
        SPL_LOG_ERROR(Streaming, "Nothing can be registered: {}", prepared.GetMessage());
        return;
    }
    Enter(StreamingStage::RegisterEarly);
}

void StreamingManager::RunGamePatches()
{
    // FiveM installs these for every session; we only do it when a registration needs them, so
    // a textures-and-models setup never writes to game code.
    if (HasRegistrationOf(m_plan->Late(), {AssetType::MapTypes, AssetType::MapData}))
    {
        if (const Result<void> installed = m_backend->InstallMapTypesPatches(); !installed)
        {
            SPL_LOG_ERROR(Streaming,
                          "Map types patches were not applied, so streamed .ytyp files may not "
                          "load: {}",
                          installed.GetMessage());
        }
    }
    if (HasRegistrationOf(m_plan->Late(), {AssetType::MapData, AssetType::StaticBounds}))
    {
        if (const Result<void> installed = m_backend->InstallMapDataPatches(); !installed)
        {
            SPL_LOG_ERROR(Streaming,
                          "Map data patches were not applied, so streamed .ymap and .ybn files "
                          "may not appear: {}",
                          installed.GetMessage());
        }
    }
    Enter(StreamingStage::RegisterLate);
}

void StreamingManager::RunDataFiles(std::span<const PlannedDataFile> dataFiles, StreamingStage next)
{
    const std::size_t end = std::min(dataFiles.size(), m_cursor + m_options.maxDataFilesPerTick);
    for (std::size_t done = 0; m_cursor < end && !IsOverBudget(done); ++m_cursor, ++done)
    {
        const PlannedDataFile& dataFile = dataFiles[m_cursor];
        BeginWork(dataFile.resourceName, dataFile.fileName, "loading data file");

        // Loading a request whose .ytyp never made it in would load whatever the game has
        // under that name instead, or fail inside the game's mounter.
        if (dataFile.matchesStreamedAsset && m_registry.Find(dataFile.fileName) == nullptr)
        {
            ++m_dataFileTotals.skipped;
            SPL_LOG_WARNING(
                Streaming, "{} '{}' from '{}' is not loaded because '{}' was not registered",
                dataFile.type, dataFile.relativePath, dataFile.resourceName, dataFile.fileName);
            continue;
        }

        const Result<std::string> loaded = m_backend->LoadDataFile(dataFile);
        if (!loaded)
        {
            ++m_dataFileTotals.failed;
            SPL_LOG_ERROR(Streaming, "{} '{}' from '{}' failed to load: {}", dataFile.type,
                          dataFile.relativePath, dataFile.resourceName, loaded.GetMessage());
            continue;
        }
        ++m_dataFileTotals.loaded;
        m_registry.AddDataFile(LoadedDataFile{.owner = dataFile.owner,
                                              .resourceName = dataFile.resourceName,
                                              .type = dataFile.type,
                                              .fileName = dataFile.fileName,
                                              .entryName = loaded.GetValue()});
        SPL_LOG_DEBUG(Streaming, "Loaded data file {} {}{}", dataFile.type, loaded.GetValue(),
                      dataFile.implicit ? " (auto_request_ytyp)" : "");
    }

    if (m_cursor < dataFiles.size())
    {
        return;
    }
    if (next == StreamingStage::Done)
    {
        Finish();
        return;
    }
    if (!dataFiles.empty())
    {
        SPL_LOG_DEBUG(Streaming, "Loaded {} data file(s), skipped {}, failed {}",
                      m_dataFileTotals.loaded, m_dataFileTotals.skipped, m_dataFileTotals.failed);
    }
    FinishDataFiles();
    Enter(next);
}

void StreamingManager::FinishDataFiles()
{
    DataFileFinishRequest request;
    request.vehicleColoursLoaded =
        std::ranges::any_of(m_registry.DataFiles(), [](const LoadedDataFile& dataFile)
                            { return dataFile.type == "CARCOLS_FILE"; });
    for (const RegisteredAsset& asset : m_registry.All())
    {
        const std::size_t slash = asset.fileName.find('/');
        if (slash == std::string::npos)
        {
            continue;
        }
        std::string folder = asset.fileName.substr(0, slash);
        if (std::ranges::find(request.pedFolders, folder) == request.pedFolders.end())
        {
            request.pedFolders.push_back(std::move(folder));
        }
    }
    if (request.vehicleColoursLoaded || !request.pedFolders.empty())
    {
        m_backend->FinishDataFiles(request);
    }
}

void StreamingManager::RunManifests()
{
    const std::span<const PlannedManifest> manifests = m_plan->Manifests();
    const std::size_t end = std::min(manifests.size(), m_cursor + m_options.maxManifestsPerTick);
    for (std::size_t done = 0; m_cursor < end && !IsOverBudget(done); ++m_cursor, ++done)
    {
        const PlannedManifest& manifest = manifests[m_cursor];
        BeginWork(manifest.resourceName, manifest.fileName, "loading packfile manifest");
        const Result<PackfileManifestOutcome> loaded =
            m_backend->LoadPackfileManifest(manifest, m_registry.DataFiles());
        if (!loaded)
        {
            ++m_manifestTotals.failed;
            SPL_LOG_ERROR(Streaming, "Packfile manifest '{}' from '{}' failed to load: {}",
                          manifest.relativePath, manifest.resourceName, loaded.GetMessage());
            continue;
        }

        const PackfileManifestOutcome& outcome = loaded.GetValue();
        ++m_manifestTotals.loaded;
        for (const std::string& entryName : outcome.releasedTypeRequests)
        {
            m_registry.MarkDataFileReleased(entryName);
            ++m_manifestTotals.releasedTypeRequests;
            SPL_LOG_DEBUG(Streaming, "'{}' now owns the DLC_ITYP_REQUEST for '{}'",
                          manifest.fileName, entryName);
        }
        for (const MapDependency& dependency : outcome.mapDependencies)
        {
            m_registry.AddMapDependency(dependency);
        }
        SPL_LOG_DEBUG(Streaming,
                      "Loaded packfile manifest '{}' from '{}': {} map dependency(ies), {} type "
                      "request(s) handed over",
                      manifest.relativePath, manifest.resourceName, outcome.mapDependencies.size(),
                      outcome.releasedTypeRequests.size());
    }

    if (m_cursor < manifests.size())
    {
        return;
    }
    if (!manifests.empty())
    {
        SPL_LOG_DEBUG(Streaming, "Loaded {} packfile manifest(s), failed {}",
                      m_manifestTotals.loaded, m_manifestTotals.failed);
    }
    Enter(StreamingStage::MapReload);
}

void StreamingManager::RunMapReload()
{
    if (!m_plan->NeedsMapStoreReload())
    {
        FinishMapStage();
        return;
    }

    if (!m_backend->CanReloadMapStore())
    {
        SetBusy(false); // waiting for a loading screen can take minutes
        if (!m_mapReloadWaitLogged)
        {
            m_mapReloadWaitLogged = true;
            SPL_LOG_INFO(Streaming, "Map store reload is waiting for the loading screen or "
                                    "character switch to end");
        }
        return; // the next tick asks again
    }

    SetBusy(true);
    const std::vector<RegisteredAsset> collisions = m_registry.OfType(AssetType::StaticBounds);
    const Result<MapReloadReport> reloaded = m_backend->ReloadMapStore(collisions);
    if (!reloaded)
    {
        m_mapReloadResult = MapReloadResult::Failed;
        SPL_LOG_ERROR(Streaming,
                      "Map store reload failed, so streamed maps and collisions may not appear: {}",
                      reloaded.GetMessage());
    }
    else
    {
        const MapReloadReport& report = reloaded.GetValue();
        m_mapReloadResult = MapReloadResult::Reloaded;
        SPL_LOG_DEBUG(Streaming, "Map store reloaded in {} ms (ybn preload {} ms, {} files) by {}",
                      report.totalMs, report.preloadMs, report.collisionsPreloaded, report.method);
    }
    FinishMapStage();
}

void StreamingManager::FinishMapStage()
{
    if (m_plan->DeferredDataFiles().empty())
    {
        Finish();
        return;
    }
    Enter(StreamingStage::DeferredDataFiles);
}

void StreamingManager::Finish()
{
    if (!m_plan->DeferredDataFiles().empty())
    {
        SPL_LOG_DEBUG(Streaming, "Loaded {} data file(s) in total, skipped {}, failed {}",
                      m_dataFileTotals.loaded, m_dataFileTotals.skipped, m_dataFileTotals.failed);
    }
    Enter(StreamingStage::Done);
    SPL_LOG_DEBUG(Streaming, "Streaming ready: {} asset(s) registered ({})", m_registry.Size(),
                  DescribeTypeCounts(m_registry));
    if (!m_waitingAssets.empty())
    {
        SPL_LOG_INFO(Streaming,
                     "{} MP-layer map file(s) wait for GTA Online's map layer, and load once a "
                     "script or streaming.mp_maps enables it",
                     m_waitingAssets.size());
    }
}

bool StreamingManager::MustWaitForGameSlot(const PlannedAsset& asset) const
{
    if (asset.type != AssetType::MapData && asset.type != AssetType::StaticBounds)
    {
        return false;
    }
    const std::string_view name = WithoutCollisionLodPrefix(asset.fileName);
    const bool matches =
        std::ranges::any_of(m_options.waitForGameSlot, [&](const std::string& pattern)
                            { return util::MatchesGlob(pattern, name); });
    return matches && !m_backend->HasGameSlot(asset);
}

std::size_t StreamingManager::RegisterWaitingAssets()
{
    if (m_backend == nullptr || m_stage != StreamingStage::Done || m_waitingAssets.empty())
    {
        return 0;
    }

    std::size_t registered = 0;
    std::erase_if(m_waitingAssets,
                  [&](const PlannedAsset* waiting)
                  {
                      const PlannedAsset& asset = *waiting;
                      if (!m_backend->HasGameSlot(asset))
                      {
                          return false;
                      }
                      // Whatever happens now is final: only a missing slot is worth waiting for.
                      BeginWork(asset.resourceName, asset.fileName, "registering");
                      const RegistrationOutcome outcome = m_backend->RegisterAsset(asset);
                      --m_totals.waiting;
                      RecordOutcome(asset, outcome, m_totals);
                      if (outcome.status == RegistrationStatus::Registered)
                      {
                          ++registered;
                      }
                      return true;
                  });

    if (registered > 0)
    {
        SPL_LOG_INFO(Streaming,
                     "Registered {} MP-layer map file(s) over the game's own; {} still waiting",
                     registered, m_waitingAssets.size());
    }
    return registered;
}

std::size_t StreamingManager::ReassertRegistrations()
{
    if (m_backend == nullptr || m_stage != StreamingStage::Done)
    {
        return 0;
    }

    struct Reasserted
    {
        rage::GlobalIndex index;
        ReassertOutcome outcome;
        std::string fileName;
        std::string resourceName;
    };
    std::vector<Reasserted> reasserted;
    for (const RegisteredAsset& asset : m_registry.All())
    {
        if (std::optional<ReassertOutcome> outcome = m_backend->ReassertAsset(asset))
        {
            reasserted.push_back(Reasserted{.index = asset.globalIndex,
                                            .outcome = *outcome,
                                            .fileName = asset.fileName,
                                            .resourceName = asset.resourceName});
        }
    }

    for (const Reasserted& entry : reasserted)
    {
        m_registry.RecordDisplacedGameHandle(entry.index, entry.outcome.displaced);
        SPL_LOG_DEBUG(Streaming,
                      "The game registered its own '{}' over the one from '{}' (handle {:#010x}); "
                      "put ours back{}",
                      entry.fileName, entry.resourceName, entry.outcome.displaced.value,
                      entry.outcome.timing == OverrideTiming::AfterUnload
                          ? ", which applies once the game unloads its copy"
                          : "");
    }
    if (!reasserted.empty())
    {
        SPL_LOG_INFO(Streaming,
                     "Put back {} registration(s) the game had replaced with its own files",
                     reasserted.size());
    }
    return reasserted.size();
}

void StreamingManager::LogRegistration(const PlannedAsset& asset,
                                       const RegistrationOutcome& outcome) const
{
    const std::string_view module = GetModuleExtension(asset.type, asset.extension);
    if (!outcome.replacedHandle)
    {
        SPL_LOG_DEBUG(Streaming, "Registered '{}' from '{}' -> {} #{} handle {:#010x}",
                      asset.fileName, asset.resourceName, module, outcome.globalIndex.value,
                      outcome.handle.value);
        return;
    }
    SPL_LOG_DEBUG(Streaming,
                  "Registered '{}' from '{}' -> {} #{} handle {:#010x}, replacing the game's copy "
                  "{:#010x}{}",
                  asset.fileName, asset.resourceName, module, outcome.globalIndex.value,
                  outcome.handle.value, outcome.replacedHandle->value,
                  outcome.overrideTiming == OverrideTiming::AfterUnload
                      ? " once the game unloads it, as it is in use"
                      : "");
}

void StreamingManager::RunRegistrations(std::span<const PlannedAsset> assets, StreamingStage next)
{
    const std::size_t end = std::min(assets.size(), m_cursor + m_options.maxRegistrationsPerTick);
    for (std::size_t done = 0; m_cursor < end && !IsOverBudget(done); ++m_cursor, ++done)
    {
        const PlannedAsset& asset = assets[m_cursor];

        // The hard type gate: a type nobody has written the registration code for never
        // reaches the game, whatever the plan and the configuration say.
        if (!IsRegistrationImplemented(asset.type))
        {
            ++m_stageTotals.deferred;
            SPL_LOG_DEBUG(Streaming,
                          "'{}' from '{}' is planned but .{} registration is not "
                          "implemented yet",
                          asset.fileName, asset.resourceName, ToString(asset.type));
            continue;
        }

        // Registered now, an MP-layer file has no game copy to replace, so it would be placed as
        // a map of its own on top of the story-mode layer it was never meant to patch.
        if (MustWaitForGameSlot(asset))
        {
            ++m_stageTotals.waiting;
            m_waitingAssets.push_back(&asset);
            SPL_LOG_DEBUG(Streaming, "'{}' from '{}' waits for the game's own '{}'", asset.fileName,
                          asset.resourceName, asset.streamingName);
            continue;
        }

        BeginWork(asset.resourceName, asset.fileName, "registering");
        RecordOutcome(asset, m_backend->RegisterAsset(asset), m_stageTotals);
    }

    if (m_cursor < assets.size())
    {
        return; // the next tick picks up where this one stopped
    }

    m_totals.registered += m_stageTotals.registered;
    m_totals.skipped += m_stageTotals.skipped;
    m_totals.failed += m_stageTotals.failed;
    m_totals.deferred += m_stageTotals.deferred;
    m_totals.waiting += m_stageTotals.waiting;
    if (!assets.empty())
    {
        LogStageSummary(ToString(m_stage), m_stageTotals);
    }
    Enter(next);
}

void StreamingManager::RecordOutcome(const PlannedAsset& asset, const RegistrationOutcome& outcome,
                                     RegistrationTotals& totals)
{
    switch (outcome.status)
    {
        using enum RegistrationStatus;
    case Registered:
        ++totals.registered;
        m_registry.Add(ToRegisteredAsset(asset, outcome));
        m_registry.PushHandle(outcome.globalIndex, outcome.handle, outcome.replacedHandle);
        LogRegistration(asset, outcome);
        break;
    case Skipped:
        ++totals.skipped;
        SPL_LOG_WARNING(Streaming, "{}", outcome.message);
        break;
    case KeptGameAsset:
        // The user asked for exactly this, so it is not something to warn about.
        ++totals.skipped;
        SPL_LOG_DEBUG(Streaming, "{}", outcome.message);
        break;
    case Failed:
        // One bad asset never stops the rest: the game is perfectly happy without it.
        ++totals.failed;
        SPL_LOG_ERROR(Streaming, "{}", outcome.message);
        break;
    }
}

void StreamingManager::LogStageSummary(std::string_view stageName,
                                       const RegistrationTotals& totals) const
{
    SPL_LOG_DEBUG(
        Streaming, "Registered {} asset(s) in {}, skipped {}, failed {}{}{}", totals.registered,
        stageName, totals.skipped, totals.failed,
        totals.deferred > 0
            ? fmt::format(", {} of a type this version cannot register yet", totals.deferred)
            : std::string{},
        totals.waiting > 0 ? fmt::format(", {} waiting for the game's map layer", totals.waiting)
                           : std::string{});
}
} // namespace spl::streaming
