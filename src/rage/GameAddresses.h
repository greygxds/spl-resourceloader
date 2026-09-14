#pragma once

#include <cstdint>

#include "core/Result.h"
#include "rage/AddressResolver.h"

namespace spl::rage
{
/// The resolved addresses the bridge calls through, as plain typed fields. Everything in
/// here comes from ResolvedSignatures, so a field only exists once the signature table has
/// a row for it.
struct GameAddresses
{
    /// rage::strStreamingInfoManager::sm_instance, the static manager object itself.
    uintptr_t streamingInfoManagerInstance = 0;

    /// strStreamingModuleMgr::GetModuleFromExtension(this, const char* extension).
    uintptr_t streamingModuleMgrGetModuleFromExtension = 0;

    /// strStreamingModuleMgr::GetModule(this, int index). Optional: diagnostics only.
    uintptr_t streamingModuleMgrGetModule = 0;

    /// rage::fiDevice::GetDevice(const char* path, bool allowRoot).
    uintptr_t fiDeviceGetDevice = 0;

    /// The rage::fiDeviceRelative vtable, copied into the device we construct ourselves.
    uintptr_t fiDeviceRelativeVftable = 0;

    /// fiDeviceRelative::SetPath(this, const char* root, bool allowRoot, fiDevice* base).
    uintptr_t fiDeviceRelativeSetPath = 0;

    /// fiDeviceRelative::Mount(this, const char* mountPoint, bool allowRoot).
    uintptr_t fiDeviceRelativeMount = 0;

    /// rage::fiDevice::Unmount(const char* mountPoint). Optional: nothing unmounts yet.
    uintptr_t fiDeviceUnmount = 0;

    /// RegisterRawStreamingFile(uint32_t* outIndex, const char* vfsPath, bool, const char*
    /// registerAs, bool errorIfFailed).
    uintptr_t registerRawStreamingFile = 0;

    /// pgRawStreamer::GetInstance(). Optional: diagnostics and overrides.
    uintptr_t pgRawStreamerGetInstance = 0;

    /// pgRawStreamer::GetEntryNameToBuffer(this, uint16_t index, char* buffer, int length).
    /// Optional: read, never called, for the offset of the entry list overrides need.
    uintptr_t pgRawStreamerGetEntryNameToBuffer = 0;

    /// strStreamingInfoManager::RequestObject(this, uint32_t index, int flags). Optional.
    uintptr_t streamingInfoManagerRequestObject = 0;

    /// strStreamingInfoManager::LoadAllRequestedObjects(bool). Optional.
    uintptr_t streamingInfoManagerLoadAllRequestedObjects = 0;

    /// strStreamingInfoManager::ReleaseObject(this, uint32_t index). Optional.
    uintptr_t streamingInfoManagerReleaseObject = 0;

    /// CDataFileMgr's type table: DataFileTypeEnumEntryView rows, ended by {0, 0xFFFFFFFF}.
    uintptr_t dataFileTypeTable = 0;

    /// CDataFileMount::sm_Interfaces, one CDataFileMountInterface* per data-file type index.
    uintptr_t dataFileMountInterfaces = 0;

    /// The two-byte "jz" that keeps raw #typ files from loading. Nopped by GamePatches.
    uintptr_t rawMapTypesLoadingCheck = 0;

    /// The fwMapTypesStore vtable, whose "should async place" slot GamePatches replaces.
    uintptr_t mapTypesStoreVftable = 0;

    // Maps. All optional: zero means map support is unavailable on this build.

    /// The fwMapDataStore vtable, whose "should async place" slot GamePatches replaces.
    uintptr_t mapDataStoreVftable = 0;

    /// The two-byte "jnz" that keeps box streamers from taking new bounds. Nopped by GamePatches.
    uintptr_t boxStreamerBoundsReconfig = 0;

    /// The static holding CInteriorProxy's atPool pointer. Optional: the console's map report.
    uintptr_t interiorProxyPool = 0;

    /// CFileLoader::LoadChangeSet(void* changeSet, void* scratch, uint32_t* hash), patched for
    /// the duration of one map-store rebuild.
    uintptr_t fileLoaderLoadChangeSet = 0;

    /// ReloadMapIfNeeded(), run after the change-set replay.
    uintptr_t reloadMapIfNeeded = 0;

    /// Address of the CExtraContentManager pointer (a pointer to the pointer).
    uintptr_t extraContentManagerInstance = 0;

    /// CExtraContentManager::DisableContentGroup(this, uint32_t groupHash).
    uintptr_t extraContentManagerDisableContentGroup = 0;

    /// CExtraContentManager::EnableContentGroup(this, uint32_t groupHash).
    uintptr_t extraContentManagerEnableContentGroup = 0;

    /// ClearContentCache(int).
    uintptr_t extraContentManagerClearContentCache = 0;

    /// rage::fiDevice::MountGlobal(const char* mountPoint, fiDevice* device, bool allowRoot).
    uintptr_t fiDeviceMountGlobal = 0;

    /// The game's packfile manifest chunk object.
    uintptr_t manifestChunk = 0;

    /// LoadPackfileManifest(void* chunk, void* packfile, const char* tagName).
    uintptr_t loadPackfileManifest = 0;

    /// InitManifestChunk(void* chunk), LoadManifestChunk(void* chunk), ClearManifestChunk(void*).
    uintptr_t initManifestChunk = 0;
    uintptr_t loadManifestChunk = 0;
    uintptr_t clearManifestChunk = 0;

    // Game file overrides. All optional: zero means that kind is unavailable on this build.

    /// fwMapTypes::ConstructArchetypes(void* mapTypes, int32_t localSlot), hooked.
    uintptr_t mapTypesConstructArchetypes = 0;

    /// fwArchetypeManager::FreeArchetypes(int32_t localSlot).
    uintptr_t archetypeManagerFreeArchetypes = 0;

    /// fwStaticBoundsStore::ModifyHierarchyStatus(void* store, int32_t localSlot, int32_t), hooked.
    uintptr_t staticBoundsStoreModifyHierarchyStatus = 0;

    /// fwMapDataStore::ModifyHierarchyStatusRecursive(void* store, int32_t localSlot, int32_t),
    /// hooked.
    uintptr_t mapDataStoreModifyHierarchyStatus = 0;

    /// The "E8 rel32" that adds a slot to a DLC packfile's dependency map, redirected.
    uintptr_t packfileDependencyAddMapBoolEntryCall = 0;

    // Content that needs more than a registration. All optional.

    /// CScaleformStore::InitGfxTexture(int32_t localSlot, const char* name), for a new .gfx slot.
    uintptr_t scaleformStoreInitGfxTexture = 0;

    /// CDataFileMgr::AddPackfile(DataFile* entry), the middle of an RPF_FILE mount.
    uintptr_t dataFileMgrAddPackfile = 0;

    /// CVehicleModelInfo::InitPaintRamps(), after a CARCOLS_FILE on build 2545 and later.
    uintptr_t vehicleModelInfoInitPaintRamps = 0;

    /// fwArchetypeManager's atArray of archetype factories; index 6 makes peds.
    uintptr_t archetypeFactories = 0;

    /// CPedModelInfoFactory::GetAllArchetypes(void* factory, atArray<CPedModelInfo*>* out).
    uintptr_t pedModelInfoFactoryGetAllArchetypes = 0;

    // Starting with the game. All optional.

    /// rage::gameSkeleton::RunInitFunctions(gameSkeleton* self, int32_t initType), hooked.
    uintptr_t gameSkeletonRunInitFunctions = 0;

    /// The "E8 rel32" that runs the game's initial device mounts, redirected.
    uintptr_t fiDeviceInitialMountCall = 0;

    /// The "E8 rel32" calls of CDataFileMgr::LoadDat(mgr, name, enabled) for the level, and of
    /// LoadDefDat for the content XML, redirected.
    uintptr_t dataFileMgrLoadDatCall = 0;
    uintptr_t dataFileMgrLoadDefDatCall = 0;

    /// The "E8 rel32" that sorts update:/ relative devices first, nopped.
    uintptr_t fiDeviceSortRelativeDevicesCall = 0;

    /// The imm32 holding the game's non-DLC mount limit, raised.
    uintptr_t fiDeviceMountLimit = 0;

    // Memory extensions. All optional.

    /// The texture VRAM budget table, rows of four uint64 budgets, rewritten by MemoryBudget.
    uintptr_t textureBudgetTable = 0;

    /// The graphics menu's texture memory estimate, (void* self, int quality, void* settings),
    /// hooked.
    uintptr_t getTextureVideoMemoryUsage = 0;

    /// How much memory the streamer may still use, (void* self), hooked.
    uintptr_t getAvailableMemoryForStreamer = 0;

    /// The imm32s of the grcResourceCache pool size and its limit, raised.
    uintptr_t resourceCachePoolSize = 0;
    uintptr_t resourceCachePoolLimit = 0;

    /// The imm32 of the streaming allocator's reservation, raised.
    uintptr_t streamingAllocatorReservation = 0;

    /// Copies the addresses this struct needs out of a resolve pass. An error means a
    /// required signature is missing, and the bridge must stay disabled.
    [[nodiscard]] static Result<GameAddresses> Build(const ResolvedSignatures& resolved);
};
} // namespace spl::rage
