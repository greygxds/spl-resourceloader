#include "rage/GameAddresses.h"

#include <string_view>

namespace spl::rage
{
namespace
{
/// Required rows have already been reported by the resolver, so a miss here only has to stop
/// the bridge, not explain itself twice.
[[nodiscard]] uintptr_t Take(const ResolvedSignatures& resolved, std::string_view name)
{
    return resolved.Find(name).value_or(0);
}
} // namespace

Result<GameAddresses> GameAddresses::Build(const ResolvedSignatures& resolved)
{
    if (!resolved.IsComplete())
    {
        return MakeError(ErrorCode::NotFound, "{} signature(s) did not resolve",
                         resolved.failures.size());
    }

    return GameAddresses{
        .streamingInfoManagerInstance = Take(resolved, "strStreamingInfoManager::sm_instance"),
        .streamingModuleMgrGetModuleFromExtension =
            Take(resolved, "strStreamingModuleMgr::GetModuleFromExtension"),
        .streamingModuleMgrGetModule = Take(resolved, "strStreamingModuleMgr::GetModule"),
        .fiDeviceGetDevice = Take(resolved, "rage::fiDevice::GetDevice"),
        .fiDeviceRelativeVftable = Take(resolved, "rage::fiDeviceRelative::vftable"),
        .fiDeviceRelativeSetPath = Take(resolved, "rage::fiDeviceRelative::SetPath"),
        .fiDeviceRelativeMount = Take(resolved, "rage::fiDeviceRelative::Mount"),
        .fiDeviceUnmount = Take(resolved, "rage::fiDevice::Unmount"),
        .registerRawStreamingFile = Take(resolved, "rage::RegisterRawStreamingFile"),
        .pgRawStreamerGetInstance = Take(resolved, "rage::pgRawStreamer::GetInstance"),
        .pgRawStreamerGetEntryNameToBuffer =
            Take(resolved, "rage::pgRawStreamer::GetEntryNameToBuffer"),
        .streamingInfoManagerRequestObject =
            Take(resolved, "strStreamingInfoManager::RequestObject"),
        .streamingInfoManagerLoadAllRequestedObjects =
            Take(resolved, "strStreamingInfoManager::LoadAllRequestedObjects"),
        .streamingInfoManagerReleaseObject =
            Take(resolved, "strStreamingInfoManager::ReleaseObject"),
        .dataFileTypeTable = Take(resolved, "CDataFileMgr::sm_TypeTable"),
        .dataFileMountInterfaces = Take(resolved, "CDataFileMount::sm_Interfaces"),
        .rawMapTypesLoadingCheck = Take(resolved, "RawMapTypesLoadingCheck"),
        .mapTypesStoreVftable = Take(resolved, "fwMapTypesStore::vftable"),
        .mapDataStoreVftable = Take(resolved, "fwMapDataStore::vftable"),
        .boxStreamerBoundsReconfig = Take(resolved, "BoxStreamerBoundsReconfig"),
        .interiorProxyPool = Take(resolved, "CInteriorProxy::sm_pPool"),
        .fileLoaderLoadChangeSet = Take(resolved, "CFileLoader::LoadChangeSet"),
        .reloadMapIfNeeded = Take(resolved, "ReloadMapIfNeeded"),
        .extraContentManagerInstance = Take(resolved, "CExtraContentManager::sm_instance"),
        .extraContentManagerDisableContentGroup =
            Take(resolved, "CExtraContentManager::DisableContentGroup"),
        .extraContentManagerEnableContentGroup =
            Take(resolved, "CExtraContentManager::EnableContentGroup"),
        .extraContentManagerClearContentCache =
            Take(resolved, "CExtraContentManager::ClearContentCache"),
        .fiDeviceMountGlobal = Take(resolved, "rage::fiDevice::MountGlobal"),
        .manifestChunk = Take(resolved, "ManifestChunk"),
        .loadPackfileManifest = Take(resolved, "LoadPackfileManifest"),
        .initManifestChunk = Take(resolved, "InitManifestChunk"),
        .loadManifestChunk = Take(resolved, "LoadManifestChunk"),
        .clearManifestChunk = Take(resolved, "ClearManifestChunk"),
        .mapTypesConstructArchetypes = Take(resolved, "fwMapTypes::ConstructArchetypes"),
        .archetypeManagerFreeArchetypes = Take(resolved, "fwArchetypeManager::FreeArchetypes"),
        .staticBoundsStoreModifyHierarchyStatus =
            Take(resolved, "fwStaticBoundsStore::ModifyHierarchyStatus"),
        .mapDataStoreModifyHierarchyStatus =
            Take(resolved, "fwMapDataStore::ModifyHierarchyStatusRecursive"),
        .packfileDependencyAddMapBoolEntryCall =
            Take(resolved, "PackfileDependencyAddMapBoolEntry"),
        .scaleformStoreInitGfxTexture = Take(resolved, "CScaleformStore::InitGfxTexture"),
        .dataFileMgrAddPackfile = Take(resolved, "CDataFileMgr::AddPackfile"),
        .vehicleModelInfoInitPaintRamps = Take(resolved, "CVehicleModelInfo::InitPaintRamps"),
        .archetypeFactories = Take(resolved, "fwArchetypeManager::ms_ArchetypeFactories"),
        .pedModelInfoFactoryGetAllArchetypes =
            Take(resolved, "CPedModelInfoFactory::GetAllArchetypes"),
        .gameSkeletonRunInitFunctions = Take(resolved, "rage::gameSkeleton::RunInitFunctions"),
        .fiDeviceInitialMountCall = Take(resolved, "rage::fiDevice::InitialMountCall"),
        .dataFileMgrLoadDatCall = Take(resolved, "CDataFileMgr::LoadDatCall"),
        .dataFileMgrLoadDefDatCall = Take(resolved, "CDataFileMgr::LoadDefDatCall"),
        .fiDeviceSortRelativeDevicesCall =
            Take(resolved, "rage::fiDevice::SortRelativeDevicesCall"),
        .fiDeviceMountLimit = Take(resolved, "rage::fiDevice::MountLimit"),
        .textureBudgetTable = Take(resolved, "TextureBudgetTable"),
        .getTextureVideoMemoryUsage = Take(resolved, "GetTextureVideoMemoryUsage"),
        .getAvailableMemoryForStreamer = Take(resolved, "GetAvailableMemoryForStreamer"),
        .resourceCachePoolSize = Take(resolved, "ResourceCachePoolSize"),
        .resourceCachePoolLimit = Take(resolved, "ResourceCachePoolLimit"),
        .streamingAllocatorReservation = Take(resolved, "StreamingAllocatorReservation"),
    };
}
} // namespace spl::rage
