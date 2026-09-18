#include "rage/StreamingInterface.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/VirtualCall.h"
#include "streaming/AssetType.h"

namespace spl::rage
{
namespace
{
using GetModuleFromExtensionFn = void* (*)(void* moduleMgr, const char* extension);

/// The game writes the new global index through outIndex and returns the same pointer; the
/// return value carries nothing extra (FiveM gta-streaming-five/src/Streaming.cpp:208).
using RegisterRawStreamingFileFn = uint32_t* (*)(uint32_t* outIndex, const char* vfsPath,
                                                 bool unkTrue, const char* registerAs,
                                                 bool errorIfFailed);

/// A vanilla texture dictionary that every build ships: scripts draw frontend sprites from
/// it. Finding its slot proves both the module lookup and the vtable shift.
constexpr std::string_view kKnownAssetName = "commonmenu";

/// Deliberately absent, so FindSlot has to answer "no" without creating anything.
constexpr std::string_view kMissingAssetName = "spl_definitely_missing_name";

/// The modules the loader will register into, so all of them must exist before we go on.
constexpr std::array<std::string_view, 7> kRequiredModules = {"ytd",  "ydr",  "ydd", "yft",
                                                              "ymap", "ytyp", "ybn"};

/// Extra stores worth seeing in a dump even though nothing registers into them yet. Not "rpf":
/// the packfile store has no asset pool where the others do, and counting it faulted on 3889.
constexpr std::array<std::string_view, 5> kExtraDumpModules = {"ycd", "ynv", "ynd", "ypt", "ymt"};

/// A live game has far more than a hundred thousand entries; build 3411 with every DLC reports
/// 2 709 473. A count below the minimum means we are not looking at the streaming manager. Heavily
/// modded games go well past the typical maximum (one reported 10 949 098), so exceeding it is
/// only logged; the module and slot checks that follow still prove the layout.
constexpr int32_t kMinEntryCount = 100000;
constexpr int32_t kTypicalMaxEntryCount = 16000000;
constexpr uint16_t kMinModuleCount = 20;
constexpr uint16_t kMaxModuleCount = 64;

[[nodiscard]] std::string FormatAddress(const memory::Module& image, uintptr_t address)
{
    if (!image.Contains(address))
    {
        return fmt::format("{:#x}", address);
    }
    return fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase());
}

/// The extensions DumpModules walks: everything the asset-type table maps to a store, then
/// the diagnostic extras that are not in it.
[[nodiscard]] std::vector<std::string_view> DumpExtensions()
{
    std::vector<std::string_view> extensions;
    for (const streaming::AssetTypeInfo& info : streaming::GetAssetTypes())
    {
        if (!info.moduleExtension.empty())
        {
            extensions.push_back(info.moduleExtension);
        }
    }
    for (std::string_view extension : kExtraDumpModules)
    {
        if (std::ranges::find(extensions, extension) == extensions.end())
        {
            extensions.push_back(extension);
        }
    }
    return extensions;
}

/// One module's slice of the global index space, for the overlap check.
struct IndexRange
{
    std::string_view extension;
    uint32_t begin = 0;
    uint32_t end = 0;
};
} // namespace

uintptr_t StreamingModule::GetVtable() const
{
    return GetVtableAddress(m_module);
}

uint32_t StreamingModule::BaseIndex() const
{
    return *reinterpret_cast<const uint32_t*>(m_module + StreamingModuleLayout::kBaseIndex);
}

std::optional<LocalSlot> StreamingModule::FindSlot(std::string_view name) const
{
    using FindSlotFn = uint32_t* (*)(void* self, uint32_t* outSlot, const char* name);
    const std::string terminated(name); // the game takes a C string, a view has no terminator

    uint32_t slot = kInvalidSlotRaw;
    const bool called =
        SafeCall("strStreamingModule::FindSlot",
                 [&]
                 {
                     const auto findSlot = GetVirtualFunction<FindSlotFn>(
                         m_module, StreamingModuleLayout::kSlotFindSlot + m_vtableShift);
                     findSlot(reinterpret_cast<void*>(m_module), &slot, terminated.c_str());
                 });
    if (!called || slot == kInvalidSlotRaw)
    {
        return std::nullopt;
    }
    return LocalSlot{slot};
}

const atPoolView* StreamingModule::GetPool() const
{
    if (m_module == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<const atPoolView*>(m_module + StreamingModuleLayout::kAssetPool);
}

std::optional<int32_t> StreamingModule::GetNumRefs(LocalSlot slot) const
{
    using GetNumRefsFn = int32_t (*)(void* self, uint32_t slot);
    if (m_module == 0 || HasFaulted())
    {
        return std::nullopt;
    }
    return SafeCall("strStreamingModule::GetNumRefs",
                    [&]
                    {
                        const auto getNumRefs = GetVirtualFunction<GetNumRefsFn>(
                            m_module, StreamingModuleLayout::kSlotGetNumRefs + m_vtableShift);
                        return getNumRefs(reinterpret_cast<void*>(m_module), slot.value);
                    });
}

uintptr_t StreamingModule::GetAssetFlagsAddress(LocalSlot slot) const
{
    const atPoolView* pool = GetPool();
    if (pool == nullptr || HasFaulted())
    {
        return 0;
    }
    const std::optional<uintptr_t> address =
        SafeCall("fwAssetStore::GetAt",
                 [&]() -> uintptr_t
                 {
                     if (pool->data == nullptr || pool->flags == nullptr ||
                         slot.value >= pool->size || pool->flags[slot.value] < 0)
                     {
                         return 0;
                     }
                     return reinterpret_cast<uintptr_t>(pool->data) +
                            static_cast<uintptr_t>(slot.value) * pool->entrySize +
                            StreamingModuleLayout::kAssetDefFlags;
                 });
    return address.value_or(0);
}

std::optional<uint16_t> StreamingModule::GetAssetFlags(LocalSlot slot) const
{
    const uintptr_t address = GetAssetFlagsAddress(slot);
    if (address == 0)
    {
        return std::nullopt;
    }
    return SafeCall("fwAssetDef::flags",
                    [&] { return *reinterpret_cast<const uint16_t*>(address); });
}

bool StreamingModule::SetAssetFlags(LocalSlot slot, uint16_t flags) const
{
    const uintptr_t address = GetAssetFlagsAddress(slot);
    if (address == 0)
    {
        return false;
    }
    return SafeCall("fwAssetDef::flags", [&] { *reinterpret_cast<uint16_t*>(address) = flags; });
}

uint32_t StreamingModule::PoolSize() const
{
    const atPoolView* pool = GetPool();
    if (pool == nullptr || !IsReadableMemory(reinterpret_cast<uintptr_t>(pool), sizeof(atPoolView)))
    {
        return 0;
    }
    return pool->size;
}

uint32_t StreamingModule::PoolUsed() const
{
    const atPoolView* pool = GetPool();
    if (pool == nullptr || HasFaulted() ||
        !IsReadableMemory(reinterpret_cast<uintptr_t>(pool), sizeof(atPoolView)) ||
        pool->flags == nullptr)
    {
        return 0;
    }
    // A store without a real pool (a wrong module, a diagnostic extra) must not fault here: a
    // caught fault stops every game call for the session.
    if (!IsReadableMemory(reinterpret_cast<uintptr_t>(pool->flags), pool->size))
    {
        return 0;
    }
    const std::optional<uint32_t> used =
        SafeCall("fwBasePool::GetCount",
                 [&]
                 {
                     uint32_t count = 0;
                     for (uint32_t index = 0; index < pool->size; ++index)
                     {
                         count += pool->flags[index] >= 0 ? 1 : 0; // negative means free
                     }
                     return count;
                 });
    return used.value_or(0);
}

Result<void> StreamingInterface::Initialize(const GameAddresses& addresses, const GameBuild& build)
{
    m_manager = addresses.streamingInfoManagerInstance;
    m_getModuleFromExtension = addresses.streamingModuleMgrGetModuleFromExtension;
    m_getModuleByIndex = addresses.streamingModuleMgrGetModule;
    m_registerRawStreamingFile = addresses.registerRawStreamingFile;
    m_requestObject = addresses.streamingInfoManagerRequestObject;
    m_loadAllRequestedObjects = addresses.streamingInfoManagerLoadAllRequestedObjects;
    m_releaseObject = addresses.streamingInfoManagerReleaseObject;
    m_vtableShift = StreamingModuleLayout::VtableShift(build.build);

    if (m_manager == 0 || m_getModuleFromExtension == 0 || m_registerRawStreamingFile == 0)
    {
        return MakeError(ErrorCode::Unavailable, "the streaming addresses did not resolve");
    }
    SPL_LOG_DEBUG(Rage, "Streaming module vtables shift by {} slot(s) on build {}", m_vtableShift,
                  build.build);
    return {};
}

const strStreamingInfoManagerView* StreamingInterface::GetManager() const
{
    return reinterpret_cast<const strStreamingInfoManagerView*>(m_manager);
}

uint32_t StreamingInterface::EntryCount() const
{
    if (m_manager == 0)
    {
        return 0;
    }
    const int32_t count = GetManager()->numEntries;
    return count > 0 ? static_cast<uint32_t>(count) : 0;
}

uint32_t StreamingInterface::ModuleCount() const
{
    if (m_manager == 0)
    {
        return 0;
    }
    return GetManager()->moduleMgr.modules.count;
}

std::optional<StreamingDataEntry> StreamingInterface::GetEntry(GlobalIndex index) const
{
    if (index.value >= EntryCount())
    {
        return std::nullopt;
    }
    const StreamingDataEntry* const entries = GetManager()->entries;
    if (entries == nullptr)
    {
        return std::nullopt;
    }
    return SafeCall("strStreamingInfoManager::Entries", [&] { return entries[index.value]; });
}

bool StreamingInterface::SetHandle(GlobalIndex index, StreamingHandle handle) const
{
    if (index.value >= EntryCount() || HasFaulted())
    {
        return false;
    }
    StreamingDataEntry* const entries = GetManager()->entries;
    if (entries == nullptr)
    {
        return false;
    }
    return SafeCall("strStreamingInfoManager::Entries",
                    [&] { entries[index.value].handle = handle.value; });
}

std::optional<StreamingModule> StreamingInterface::GetModule(std::string_view extension) const
{
    if (m_manager == 0 || m_getModuleFromExtension == 0 || HasFaulted())
    {
        return std::nullopt;
    }

    const std::string terminated(extension);
    void* const moduleMgr =
        reinterpret_cast<void*>(m_manager + offsetof(strStreamingInfoManagerView, moduleMgr));
    const auto getModule = reinterpret_cast<GetModuleFromExtensionFn>(m_getModuleFromExtension);

    const std::optional<void*> module =
        SafeCall("strStreamingModuleMgr::GetModuleFromExtension",
                 [&] { return getModule(moduleMgr, terminated.c_str()); });
    if (!module || *module == nullptr)
    {
        return std::nullopt;
    }
    return StreamingModule(reinterpret_cast<uintptr_t>(*module), m_vtableShift);
}

std::optional<GlobalIndex> StreamingInterface::RegisterRawFile(std::string_view vfsPath,
                                                               std::string_view fileName) const
{
    if (m_registerRawStreamingFile == 0 || HasFaulted())
    {
        return std::nullopt;
    }

    const std::string path(vfsPath);
    const std::string name(fileName);
    const auto registerRawFile =
        reinterpret_cast<RegisterRawStreamingFileFn>(m_registerRawStreamingFile);

    uint32_t index = kInvalidSlotRaw; // what FiveM seeds it with, and how failure is signalled
    const bool called =
        SafeCall("rage::RegisterRawStreamingFile",
                 [&] { registerRawFile(&index, path.c_str(), true, name.c_str(), false); });
    if (!called || index == kInvalidSlotRaw)
    {
        return std::nullopt;
    }
    return GlobalIndex{index};
}

bool StreamingInterface::RequestObject(GlobalIndex index, int flags) const
{
    using RequestObjectFn = void (*)(void* manager, uint32_t index, int flags);
    if (m_requestObject == 0 || HasFaulted())
    {
        return false;
    }
    const auto request = reinterpret_cast<RequestObjectFn>(m_requestObject);
    return SafeCall("strStreamingInfoManager::RequestObject",
                    [&] { request(reinterpret_cast<void*>(m_manager), index.value, flags); });
}

bool StreamingInterface::LoadAllRequestedObjects() const
{
    using LoadAllRequestedObjectsFn = void (*)(bool priorityOnly);
    if (m_loadAllRequestedObjects == 0 || HasFaulted())
    {
        return false;
    }
    const auto loadAll = reinterpret_cast<LoadAllRequestedObjectsFn>(m_loadAllRequestedObjects);
    return SafeCall("strStreamingInfoManager::LoadAllRequestedObjects", [&] { loadAll(false); });
}

bool StreamingInterface::ReleaseObject(GlobalIndex index) const
{
    using ReleaseObjectFn = bool (*)(void* manager, uint32_t index);
    if (m_releaseObject == 0 || HasFaulted())
    {
        return false;
    }
    const auto release = reinterpret_cast<ReleaseObjectFn>(m_releaseObject);
    const std::optional<bool> released =
        SafeCall("strStreamingInfoManager::ReleaseObject",
                 [&] { return release(reinterpret_cast<void*>(m_manager), index.value); });
    return released.value_or(false);
}

Result<void> StreamingInterface::Verify(const memory::Module& image) const
{
    if (!image.Contains(m_manager))
    {
        return MakeError(ErrorCode::NotFound, "the streaming manager at {:#x} is outside {}",
                         m_manager, image.GetFileName());
    }

    const strStreamingInfoManagerView& manager = *GetManager();
    if (manager.numEntries < kMinEntryCount)
    {
        return MakeError(ErrorCode::NotFound,
                         "the streaming manager reports {} entries, fewer than the plausible "
                         "minimum of {}",
                         manager.numEntries, kMinEntryCount);
    }
    if (manager.numEntries > kTypicalMaxEntryCount)
    {
        SPL_LOG_WARNING(Rage,
                        "the streaming manager reports {} entries, more than the typical {}; "
                        "continuing, as heavily modded games can exceed it",
                        manager.numEntries, kTypicalMaxEntryCount);
    }

    const auto entries = reinterpret_cast<uintptr_t>(manager.entries);
    if (entries == 0 || image.Contains(entries))
    {
        return MakeError(ErrorCode::NotFound,
                         "the streaming entry array is at {}, which is not a heap allocation",
                         FormatAddress(image, entries));
    }

    const atArrayView<void*>& modules = manager.moduleMgr.modules;
    if (modules.data == nullptr || modules.count < kMinModuleCount ||
        modules.count > kMaxModuleCount)
    {
        return MakeError(ErrorCode::NotFound,
                         "the module manager reports {} module(s), expected {}..{}", modules.count,
                         kMinModuleCount, kMaxModuleCount);
    }
    for (uint16_t index = 0; index < modules.count; ++index)
    {
        void* const module = modules.data[index];
        if (module == nullptr)
        {
            return MakeError(ErrorCode::NotFound, "streaming module {} is null", index);
        }
        const uintptr_t vtable = GetVtableAddress(reinterpret_cast<uintptr_t>(module));
        if (!image.Contains(vtable))
        {
            return MakeError(ErrorCode::NotFound,
                             "streaming module {} has vtable {}, which is outside {}", index,
                             FormatAddress(image, vtable), image.GetFileName());
        }
    }

    std::vector<IndexRange> ranges;
    for (std::string_view extension : kRequiredModules)
    {
        const std::optional<StreamingModule> module = GetModule(extension);
        if (!module)
        {
            return MakeError(ErrorCode::NotFound, "the game has no '{}' streaming module",
                             extension);
        }
        const uint32_t baseIndex = module->BaseIndex();
        if (baseIndex >= static_cast<uint32_t>(manager.numEntries))
        {
            return MakeError(ErrorCode::NotFound,
                             "the '{}' module starts at global index {}, past the {} entries",
                             extension, baseIndex, manager.numEntries);
        }
        ranges.push_back(IndexRange{extension, baseIndex, baseIndex + module->PoolSize()});
    }

    std::ranges::sort(ranges, {}, &IndexRange::begin);
    for (size_t index = 1; index < ranges.size(); ++index)
    {
        if (ranges[index].begin < ranges[index - 1].end)
        {
            return MakeError(ErrorCode::Ambiguous, "the '{}' and '{}' index ranges overlap at {}",
                             ranges[index - 1].extension, ranges[index].extension,
                             ranges[index].begin);
        }
    }

    const std::optional<StreamingModule> textures = GetModule("ytd");
    if (!textures)
    {
        return MakeError(ErrorCode::Unavailable, "the 'ytd' module stopped resolving mid-check");
    }
    if (!textures->FindSlot(kKnownAssetName))
    {
        return MakeError(ErrorCode::NotFound,
                         "the vanilla texture dictionary '{}' has no slot, so the vtable "
                         "layout for this build is wrong",
                         kKnownAssetName);
    }
    if (textures->FindSlot(kMissingAssetName))
    {
        return MakeError(ErrorCode::Ambiguous,
                         "'{}' resolved to a slot, so FindSlot is creating slots instead of "
                         "looking them up",
                         kMissingAssetName);
    }
    return {};
}

void StreamingInterface::DumpModules(const memory::Module& image) const
{
    SPL_LOG_DEBUG(Rage, "strStreamingInfoManager @ {}, {} entries, {} modules",
                  FormatAddress(image, m_manager), EntryCount(), ModuleCount());

    for (std::string_view extension : DumpExtensions())
    {
        const std::optional<StreamingModule> module = GetModule(extension);
        if (!module)
        {
            SPL_LOG_DEBUG(Rage, "  {:<4} not present", extension);
            continue;
        }
        SPL_LOG_DEBUG(Rage, "  {:<4} base={:>8} slots={:>7} used={:>7}", extension,
                      module->BaseIndex(), module->PoolSize(), module->PoolUsed());
    }
}
} // namespace spl::rage
