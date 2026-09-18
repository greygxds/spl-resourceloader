#include "rage/RageStreamingBackend.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/Natives.h"
#include "rage/StreamingInterface.h"
#include "rage/VfsPath.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/StreamingTypes.h"
#include "streaming/AssetType.h"
#include "streaming/DataFileType.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
using streaming::RegistrationOutcome;
using streaming::RegistrationStatus;

[[nodiscard]] RegistrationOutcome Failure(std::string message)
{
    return RegistrationOutcome{.status = RegistrationStatus::Failed, .message = std::move(message)};
}

[[nodiscard]] RegistrationOutcome Skip(std::string message)
{
    return RegistrationOutcome{.status = RegistrationStatus::Skipped,
                               .message = std::move(message)};
}

constexpr std::string_view kCoreTexturesFolder = "platform:/textures/";

/// The first build whose paint ramps need rebuilding after a CARCOLS_FILE (FiveM
/// LoadStreamingFile.cpp:3776).
constexpr uint32_t kPaintRampsBuild = 2545;

[[nodiscard]] std::string_view WithoutExtension(std::string_view fileName)
{
    const std::size_t dot = fileName.find_last_of('.');
    return dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
}

[[nodiscard]] bool IsMapDataType(streaming::AssetType type)
{
    return type == streaming::AssetType::MapData || type == streaming::AssetType::StaticBounds;
}
} // namespace

Result<void> RageStreamingBackend::PrepareResourceRoot(const std::filesystem::path& root)
{
    if (!m_bridge->IsReady())
    {
        return MakeError(ErrorCode::Unavailable, "the RAGE bridge is {}",
                         ToString(m_bridge->GetState()));
    }
    return m_bridge->Files().MountResourcesRoot(root);
}

bool RageStreamingBackend::HasGameSlot(const streaming::PlannedAsset& asset)
{
    if (!m_bridge->IsReady())
    {
        return false;
    }
    const std::optional<StreamingModule> module =
        m_bridge->Streaming().GetModule(streaming::GetModuleExtension(asset.type, asset.extension));
    if (!module)
    {
        return false;
    }
    const std::optional<LocalSlot> slot = module->FindSlot(asset.streamingName);
    if (!slot)
    {
        return false;
    }
    const std::optional<StreamingDataEntry> entry =
        m_bridge->Streaming().GetEntry(GlobalIndex{module->BaseIndex() + slot->value});
    // A loose file's slot is not the game's: taking it over would not replace anything the
    // game placed.
    return entry && entry->handle != 0 && !IsRawHandle(StreamingHandle{entry->handle});
}

RegistrationOutcome RageStreamingBackend::RegisterAsset(const streaming::PlannedAsset& asset)
{
    if (!m_bridge->IsReady())
    {
        return Failure(fmt::format("'{}' was not registered: the RAGE bridge is {}", asset.fileName,
                                   ToString(m_bridge->GetState())));
    }

    const std::string_view extension = streaming::GetModuleExtension(asset.type, asset.extension);
    const std::optional<StreamingModule> module = m_bridge->Streaming().GetModule(extension);
    if (!module && asset.type == streaming::AssetType::OtherModule)
    {
        return Skip(fmt::format("'{}' from '{}' was not registered: the game has no '{}' "
                                "streaming module, so it does not belong in stream/",
                                asset.relativePath, asset.resourceName, extension));
    }
    if (!module)
    {
        return Failure(fmt::format("'{}' was not registered: the game has no '{}' streaming module",
                                   asset.fileName, extension));
    }

    const std::optional<std::string> vfsPath = m_bridge->Files().ToVfsPath(asset.absolutePath);
    if (!vfsPath)
    {
        return Failure(fmt::format("'{}' was not registered: it is outside the mounted folders "
                                   "(resources and mods)",
                                   asset.fileName));
    }

    // A core texture dictionary is known by its path, so a same-named file only finds its slot
    // under that name. Replacing one is an override, hence the setting.
    std::string registrationName = asset.fileName;
    std::string streamingName = asset.streamingName;
    if (m_settings.allowOverrides && asset.type == streaming::AssetType::TextureDictionary)
    {
        if (std::optional<std::string> corePath = FindCoreTexturePath(asset.fileName))
        {
            streamingName = std::string{WithoutExtension(*corePath)};
            registrationName = std::move(*corePath);
        }
    }

    // A slot that already carries a handle is a vanilla, DLC or previously registered asset.
    // Taking it over is an override.
    const std::optional<LocalSlot> existingSlot = module->FindSlot(streamingName);
    if (existingSlot)
    {
        const GlobalIndex existing{module->BaseIndex() + existingSlot->value};
        const std::optional<StreamingDataEntry> entry = m_bridge->Streaming().GetEntry(existing);
        if (entry && entry->handle != 0)
        {
            return RegisterOverride(asset, *module, *existingSlot, *entry, *vfsPath);
        }
    }

    // A new asset takes a store slot. A full pool is a game assertion, so refuse it here with a
    // message instead; a pool that cannot be read (size 0) is left to the game. A name that
    // already has a slot reuses it: the navmesh store allocates one per grid cell up front, so
    // its pool always reads as full while every ynv still has a slot waiting.
    SlotBudget* budget = existingSlot ? nullptr : FindSlotBudget(*module);
    if (budget != nullptr && budget->used >= budget->size)
    {
        return Failure(fmt::format("'{}' from '{}' was not registered: the game's '{}' store is "
                                   "full ({} of {} slots in use)",
                                   asset.fileName, asset.resourceName, extension, budget->used,
                                   budget->size));
    }

    const std::optional<GlobalIndex> index =
        m_bridge->Streaming().RegisterRawFile(*vfsPath, registrationName);
    if (index && budget != nullptr)
    {
        ++budget->used;
    }
    if (!index)
    {
        return Failure(fmt::format("'{}' from '{}' was rejected by the game's raw streamer ('{}')",
                                   asset.fileName, asset.resourceName, *vfsPath));
    }

    const std::optional<StreamingDataEntry> entry = m_bridge->Streaming().GetEntry(*index);
    if (!entry || entry->handle == 0)
    {
        return Failure(fmt::format("'{}' from '{}' took slot {} but the game left its handle "
                                   "empty",
                                   asset.fileName, asset.resourceName, index->value));
    }

    const StreamingHandle handle{entry->handle};
    if (!IsRawHandle(handle))
    {
        return Failure(fmt::format("'{}' from '{}' landed in collection {} instead of the raw "
                                   "streamer",
                                   asset.fileName, asset.resourceName, CollectionOf(handle)));
    }

    // A Scaleform movie the game did not have needs its slot prepared, or it never draws.
    if (asset.type == streaming::AssetType::Scaleform && !existingSlot)
    {
        const LocalSlot slot{index->value - module->BaseIndex()};
        if (Result<void> prepared = m_bridge->Content().InitScaleformSlot(slot, streamingName);
            !prepared)
        {
            SPL_LOG_WARNING(Streaming, "'{}' from '{}' is registered, but may not draw: {}",
                            asset.fileName, asset.resourceName, prepared.GetMessage());
        }
    }

    return RegistrationOutcome{.status = RegistrationStatus::Registered,
                               .vfsPath = *vfsPath,
                               .globalIndex = *index,
                               .handle = handle};
}

RageStreamingBackend::SlotBudget*
RageStreamingBackend::FindSlotBudget(const StreamingModule& module)
{
    auto found = m_slotBudgets.find(module.Raw());
    if (found == m_slotBudgets.end())
    {
        const SlotBudget budget{.size = module.PoolSize(), .used = module.PoolUsed()};
        found = m_slotBudgets.emplace(module.Raw(), budget).first;
        SPL_LOG_DEBUG(Rage, "Store at {:#x}: {} of {} slots in use before registration",
                      module.Raw(), budget.used, budget.size);
        if (budget.size != 0 && budget.used * 10 >= budget.size * 9)
        {
            SPL_LOG_WARNING(Rage,
                            "Store at {:#x} is almost full before registration ({} of {} slots "
                            "in use), so resources may be refused",
                            module.Raw(), budget.used, budget.size);
        }
    }
    return found->second.size != 0 ? &found->second : nullptr;
}

RegistrationOutcome RageStreamingBackend::RegisterOverride(const streaming::PlannedAsset& asset,
                                                           const StreamingModule& module,
                                                           LocalSlot slot,
                                                           const StreamingDataEntry& entry,
                                                           const std::string& vfsPath)
{
    if (!m_settings.allowOverrides)
    {
        return RegistrationOutcome{
            .status = RegistrationStatus::KeptGameAsset,
            .message = fmt::format("'{}' from '{}' already exists in the game, which keeps its "
                                   "own (streaming.allow_overrides = false)",
                                   asset.fileName, asset.resourceName)};
    }
    if (const Result<void>& support = m_bridge->GetOverrideSupport(); !support)
    {
        return Skip(fmt::format("'{}' from '{}' would override a game asset, but this build "
                                "cannot: {}",
                                asset.fileName, asset.resourceName, support.GetMessage()));
    }

    // The hooks go in before the slot changes: once it points at our file, the game may load
    // it at any request.
    if (asset.type == streaming::AssetType::MapTypes)
    {
        if (Result<void> hooked = m_bridge->Overrides().InstallMapTypesHooks(); !hooked)
        {
            return Skip(fmt::format("'{}' from '{}' would override a game .ytyp, but its "
                                    "archetypes could not be managed: {}",
                                    asset.fileName, asset.resourceName, hooked.GetMessage()));
        }
    }
    if (IsMapDataType(asset.type))
    {
        if (Result<void> hooked = m_bridge->Overrides().InstallMapDataHooks(); !hooked)
        {
            return Skip(fmt::format("'{}' from '{}' would override game map data, but the map "
                                    "store could not be hooked: {}",
                                    asset.fileName, asset.resourceName, hooked.GetMessage()));
        }
    }

    const GlobalIndex index{module.BaseIndex() + slot.value};
    const std::optional<uint16_t> entryIndex = m_bridge->RawStreamer().GetEntryByName(vfsPath);
    if (!entryIndex)
    {
        return Failure(fmt::format("'{}' from '{}' was rejected by the game's raw streamer ('{}')",
                                   asset.fileName, asset.resourceName, vfsPath));
    }
    const std::optional<std::string> entryName = m_bridge->RawStreamer().GetEntryPath(*entryIndex);
    if (!entryName || !util::EqualsIgnoreCase(*entryName, vfsPath))
    {
        return Failure(fmt::format("'{}' from '{}' got raw entry {}, which names '{}' instead of "
                                   "'{}'; the game asset was left alone",
                                   asset.fileName, asset.resourceName, *entryIndex,
                                   entryName.value_or("nothing"), vfsPath));
    }

    const StreamingHandle ours = MakeRawHandle(*entryIndex);
    const StreamingHandle replaced{entry.handle};
    if (ours == replaced)
    {
        return Skip(fmt::format("'{}' from '{}' is already registered", asset.fileName,
                                asset.resourceName));
    }
    if (!m_bridge->Streaming().SetHandle(index, ours))
    {
        return Failure(fmt::format("'{}' from '{}' could not take over slot {}", asset.fileName,
                                   asset.resourceName, index.value));
    }
    if (IsMapDataType(asset.type))
    {
        m_bridge->Overrides().AddOverriddenMapIndex(index);
    }

    return RegistrationOutcome{.status = RegistrationStatus::Registered,
                               .vfsPath = vfsPath,
                               .globalIndex = index,
                               .handle = ours,
                               .replacedHandle = replaced,
                               .overrideTiming = ReleaseGameCopy(module, slot, index, entry)};
}

streaming::OverrideTiming RageStreamingBackend::ReleaseGameCopy(const StreamingModule& module,
                                                                LocalSlot slot, GlobalIndex index,
                                                                const StreamingDataEntry& entry)
{
    switch (LoadStateOf(entry))
    {
        using enum LoadState;
    case NotLoaded:
    case Requested: // nothing is read yet, so the request will read our file
        return streaming::OverrideTiming::NextLoad;
    case Loading:
        return streaming::OverrideTiming::AfterUnload;
    case Loaded:
        break;
    }

    // Only an unreferenced copy can go; the game frees a referenced one when it is done with it.
    const std::optional<int32_t> references = module.GetNumRefs(slot);
    if (references && *references == 0 && m_bridge->Streaming().ReleaseObject(index))
    {
        return streaming::OverrideTiming::NextLoad;
    }
    return streaming::OverrideTiming::AfterUnload;
}

std::optional<std::string>
RageStreamingBackend::FindCoreTexturePath(std::string_view fileName) const
{
    std::string path = fmt::format("{}{}", kCoreTexturesFolder, fileName);
    void* const device = m_bridge->Files().GetDevice(path, true);
    if (device == nullptr || !m_bridge->Files().GetFileAttributes(device, path))
    {
        return std::nullopt;
    }
    return path;
}

void RageStreamingBackend::ReleasePermanentMapTypes(std::string_view fileName)
{
    const std::optional<StreamingModule> module = m_bridge->Streaming().GetModule(
        streaming::GetAssetTypeInfo(streaming::AssetType::MapTypes).moduleExtension);
    if (!module)
    {
        return;
    }
    const std::optional<LocalSlot> slot = module->FindSlot(WithoutExtension(fileName));
    if (!slot)
    {
        return;
    }
    const GlobalIndex index{module->BaseIndex() + slot->value};
    const std::optional<StreamingDataEntry> entry = m_bridge->Streaming().GetEntry(index);
    // Only a slot that now points at a loose file is ours: the game's own come from archives.
    if (!entry || entry->handle == 0 || !IsRawHandle(StreamingHandle{entry->handle}))
    {
        return;
    }
    const std::optional<uint16_t> flags = module->GetAssetFlags(*slot);
    if (!flags || (*flags & StreamingModuleLayout::kAssetFlagPermanent) == 0)
    {
        return;
    }

    const auto cleared =
        static_cast<uint16_t>(*flags & ~StreamingModuleLayout::kAssetFlagsClearedToRelease);
    if (!module->SetAssetFlags(*slot, cleared))
    {
        SPL_LOG_WARNING(Streaming, "The game's permanent '{}' could not be released", fileName);
        return;
    }
    const bool released = m_bridge->Streaming().ReleaseObject(index);
    SPL_LOG_DEBUG(Streaming, "Released the game's permanent '{}'{}", fileName,
                  released ? " so the streamed one loads instead"
                           : ", but the game kept it loaded until it is unused");
}

Result<void> RageStreamingBackend::InstallMapTypesPatches()
{
    if (!m_bridge->IsReady())
    {
        return MakeError(ErrorCode::Unavailable, "the RAGE bridge is {}",
                         ToString(m_bridge->GetState()));
    }
    return m_bridge->Patches().InstallMapTypesPatches();
}

Result<std::string> RageStreamingBackend::LoadDataFile(const streaming::PlannedDataFile& dataFile)
{
    if (!m_bridge->IsReady())
    {
        return MakeError(ErrorCode::Unavailable, "the RAGE bridge is {}",
                         ToString(m_bridge->GetState()));
    }

    const std::optional<std::string> vfsPath = m_bridge->Files().ToVfsPath(dataFile.absolutePath);
    if (!vfsPath)
    {
        return MakeError(ErrorCode::NotFound, "it is outside the mounted folders");
    }
    const bool isTypeRequest = dataFile.policy == streaming::DataFilePolicy::TypeRequest;
    const std::optional<std::string> entryName = MakeDataFileEntryName(
        *vfsPath, isTypeRequest ? DataFileNaming::BaseNameIsEnough : DataFileNaming::PathOnly);
    if (!entryName)
    {
        return MakeError(ErrorCode::InvalidArgument,
                         "'{}' is longer than the {} characters the game's data-file entry holds; "
                         "move the resource to a shorter path",
                         *vfsPath, DataFileLayout::kMaxNameLength);
    }

    if (isTypeRequest && m_settings.allowOverrides)
    {
        ReleasePermanentMapTypes(dataFile.fileName);
    }

    if (dataFile.policy == streaming::DataFilePolicy::Packfile)
    {
        if (const Result<void>& support = m_bridge->GetManifestSupport(); !support)
        {
            return MakeError(ErrorCode::NotSupported, "this build cannot mount packfiles: {}",
                             support.GetMessage());
        }
        Result<DataFileEntryView*> entry =
            m_bridge->DataFiles().CreateEntry(dataFile.type, *entryName);
        if (!entry)
        {
            return entry.GetError();
        }
        if (Result<void> mounted = m_bridge->Manifests().MountPackfile(entry.GetValue()); !mounted)
        {
            return mounted.GetError();
        }
        return *entryName;
    }

    if (Result<void> loaded = m_bridge->DataFiles().Load(dataFile.type, *entryName); !loaded)
    {
        return loaded.GetError();
    }
    return *entryName;
}

void RageStreamingBackend::FinishDataFiles(const streaming::DataFileFinishRequest& request)
{
    if (!m_bridge->IsReady())
    {
        return;
    }
    if (request.vehicleColoursLoaded && IsBuildAtLeast(m_bridge->GetBuild(), kPaintRampsBuild))
    {
        if (Result<void> built = m_bridge->Content().InitVehiclePaintRamps(); !built)
        {
            SPL_LOG_WARNING(Streaming,
                            "Custom vehicle paints and liveries may look wrong: the paint ramps "
                            "were not rebuilt ({})",
                            built.GetMessage());
        }
        else
        {
            SPL_LOG_DEBUG(Streaming, "Rebuilt the vehicle paint ramps after CARCOLS_FILE");
        }
    }
    if (!request.pedFolders.empty())
    {
        const Result<std::size_t> set = m_bridge->Content().SetPedStreamFolders(request.pedFolders);
        if (!set)
        {
            SPL_LOG_WARNING(Streaming,
                            "Add-on peds streamed as '<ped>^<file>' may miss their components: {}",
                            set.GetMessage());
        }
        else if (set.GetValue() > 0)
        {
            SPL_LOG_INFO(Streaming,
                         "Set the stream folder of {} add-on ped model(s) ({} folder(s) streamed)",
                         set.GetValue(), request.pedFolders.size());
        }
    }
}

Result<void> RageStreamingBackend::InstallMapDataPatches()
{
    if (Result<void> ready = RequireReadyBridge(); !ready)
    {
        return ready;
    }
    return m_bridge->Patches().InstallMapDataPatches();
}

Result<streaming::PackfileManifestOutcome> RageStreamingBackend::LoadPackfileManifest(
    const streaming::PlannedManifest& manifest,
    std::span<const streaming::LoadedDataFile> loadedDataFiles)
{
    if (Result<void> ready = RequireReadyBridge(); !ready)
    {
        return ready.GetError();
    }
    if (const Result<void>& support = m_bridge->GetManifestSupport(); !support)
    {
        return MakeError(ErrorCode::NotSupported, "this build cannot load packfile manifests: {}",
                         support.GetMessage());
    }

    const std::optional<std::string> vfsPath = m_bridge->Files().ToVfsPath(manifest.absolutePath);
    if (!vfsPath)
    {
        return MakeError(ErrorCode::NotFound, "it is outside the mounted folders");
    }

    // The resource name is the tag the game files the manifest's entries under, as FiveM's is.
    Result<ParsedPackfileManifest> parsed =
        m_bridge->Manifests().Parse(m_bridge->Files(), *vfsPath, manifest.resourceName);
    if (!parsed)
    {
        return parsed.GetError();
    }

    streaming::PackfileManifestOutcome outcome;
    // A .ytyp the manifest describes belongs to the map store from now on; keeping it loaded as
    // a permanent DLC_ITYP_REQUEST as well would load it twice (FiveM LoadStreamingFile.cpp:2292).
    for (const ManifestDependencyRow& mapTypes : parsed.GetValue().mapTypes)
    {
        const streaming::LoadedDataFile* const request =
            streaming::FindTypeRequest(loadedDataFiles, mapTypes.name);
        if (request == nullptr ||
            std::ranges::find(outcome.releasedTypeRequests, request->entryName) !=
                outcome.releasedTypeRequests.end())
        {
            continue;
        }
        if (Result<void> unloaded = m_bridge->DataFiles().Unload(request->type, request->entryName);
            !unloaded)
        {
            SPL_LOG_WARNING(Streaming, "'{}' could not hand '{}' over to the map store: {}",
                            manifest.fileName, request->fileName, unloaded.GetMessage());
            continue;
        }
        outcome.releasedTypeRequests.push_back(request->entryName);
    }
    for (const ManifestDependencyRow& mapData : parsed.GetValue().mapData)
    {
        for (const uint32_t mapTypesHash : mapData.dependencies)
        {
            outcome.mapDependencies.push_back(
                streaming::MapDependency{.owner = manifest.owner,
                                         .mapDataHash = mapData.name,
                                         .mapTypesHash = mapTypesHash});
        }
    }

    if (Result<void> committed = m_bridge->Manifests().Commit(); !committed)
    {
        return committed.GetError();
    }
    return outcome;
}

bool RageStreamingBackend::CanReloadMapStore()
{
    if (!m_bridge->IsReady() || m_insideGameStartup)
    {
        // ReloadMapStore reports the bridge; waiting for it would never end. During startup
        // the natives cannot be called, and the end of INIT_SESSION is where FiveM reloads.
        return true;
    }
    return !IsLoadingScreenActive() && !IsPlayerSwitchInProgress();
}

Result<streaming::MapReloadReport>
RageStreamingBackend::ReloadMapStore(std::span<const streaming::RegisteredAsset> collisions)
{
    if (Result<void> ready = RequireReadyBridge(); !ready)
    {
        return ready.GetError();
    }
    if (const Result<void>& support = m_bridge->GetMapReloadSupport(); !support)
    {
        return MakeError(ErrorCode::NotSupported, "this build cannot reload the map store: {}",
                         support.GetMessage());
    }

    std::vector<GlobalIndex> indexes;
    indexes.reserve(collisions.size());
    for (const streaming::RegisteredAsset& collision : collisions)
    {
        indexes.push_back(collision.globalIndex);
    }

    const Result<MapReloadTimings> reloaded =
        m_bridge->MapStore().Reload(m_bridge->Streaming(), indexes, m_mapReloadStrategy);
    if (!reloaded)
    {
        return reloaded.GetError();
    }
    m_bridge->Patches().Dump(); // the temporary patches are gone; only the session ones remain

    const MapReloadTimings& timings = reloaded.GetValue();
    return streaming::MapReloadReport{.method = std::string{ToString(timings.method)},
                                      .collisionsPreloaded = timings.collisionsPreloaded,
                                      .preloadMs = timings.preloadMs,
                                      .totalMs = timings.totalMs};
}

Result<void> RageStreamingBackend::RequireReadyBridge() const
{
    if (!m_bridge->IsReady())
    {
        return MakeError(ErrorCode::Unavailable, "the RAGE bridge is {}",
                         ToString(m_bridge->GetState()));
    }
    return {};
}

std::optional<streaming::ReassertOutcome>
RageStreamingBackend::ReassertAsset(const streaming::RegisteredAsset& asset)
{
    if (!m_bridge->IsReady() || !m_settings.allowOverrides)
    {
        return std::nullopt;
    }
    const std::optional<StreamingDataEntry> entry =
        m_bridge->Streaming().GetEntry(asset.globalIndex);
    if (!entry || entry->handle == 0 || entry->handle == asset.handle.value)
    {
        return std::nullopt;
    }
    // Only a handle into an archive is the game's own; a loose file there is nobody's to undo.
    const StreamingHandle current{entry->handle};
    if (IsRawHandle(current))
    {
        return std::nullopt;
    }
    const std::optional<StreamingModule> module =
        m_bridge->Streaming().GetModule(asset.moduleExtension);
    if (!module)
    {
        return std::nullopt;
    }
    // A map layer the game turned off (ON_ENTER_SP) frees its slots, and one can come back
    // holding another file. Only a slot that still goes by our name is ours to take back.
    if (IsMapDataType(asset.type))
    {
        const std::optional<LocalSlot> named = module->FindSlot(WithoutExtension(asset.fileName));
        if (!named || module->BaseIndex() + named->value != asset.globalIndex.value)
        {
            return std::nullopt;
        }
    }
    if (asset.type == streaming::AssetType::MapTypes &&
        !m_bridge->Overrides().InstallMapTypesHooks())
    {
        return std::nullopt;
    }
    if (IsMapDataType(asset.type) && !m_bridge->Overrides().InstallMapDataHooks())
    {
        return std::nullopt;
    }

    if (!m_bridge->Streaming().SetHandle(asset.globalIndex, asset.handle))
    {
        return std::nullopt;
    }
    if (IsMapDataType(asset.type))
    {
        m_bridge->Overrides().AddOverriddenMapIndex(asset.globalIndex);
    }
    const LocalSlot slot{asset.globalIndex.value - module->BaseIndex()};
    return streaming::ReassertOutcome{
        .displaced = current, .timing = ReleaseGameCopy(*module, slot, asset.globalIndex, *entry)};
}

Result<void> RageStreamingBackend::UnregisterAsset(const streaming::RegisteredAsset& asset)
{
    return MakeError(ErrorCode::NotSupported, "unregistering '{}' is not supported",
                     asset.fileName);
}
} // namespace spl::rage
