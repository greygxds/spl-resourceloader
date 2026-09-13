#include "rage/OverrideHooks.h"

#include <optional>
#include <string>
#include <utility>

#include "hooking/HookManager.h"
#include "logging/Logger.h"
#include "memory/Address.h"
#include "memory/Module.h"
#include "rage/SafeCall.h"
#include "rage/types/MapStoreTypes.h"

namespace spl::rage
{
namespace
{
constexpr std::string_view kConstructArchetypesHook = "fwMapTypes::ConstructArchetypes";
constexpr std::string_view kStaticBoundsHierarchyHook =
    "fwStaticBoundsStore::ModifyHierarchyStatus";
constexpr std::string_view kMapDataHierarchyHook = "fwMapDataStore::ModifyHierarchyStatusRecursive";
constexpr std::string_view kAddMapBoolEntryPatch = "PackfileDependencyAddMapBoolEntry";

constexpr uint8_t kCallOpcode = 0xE8;

/// Creates and enables one hook, reporting which one when it does not go in.
template <typename TFn>
[[nodiscard]] Result<void> InstallHook(std::string_view name, uintptr_t target, TFn detour,
                                       TFn* original)
{
    hooking::HookManager& hooks = hooking::HookManager::Instance();
    if (hooks.FindStatus(name) == hooking::HookStatus::Enabled)
    {
        return {}; // a previous attempt got this far before something after it failed
    }
    if (Result<void> created = hooks.Create(name, target, detour, original); !created)
    {
        return created;
    }
    if (Result<void> enabled = hooks.Enable(name); !enabled)
    {
        return enabled;
    }
    const memory::Module image = memory::Module::Main();
    SPL_LOG_DEBUG(Hook, "Hook '{}' installed at {}+{:#x}", name, image.GetFileName(),
                  target - image.GetBase());
    return {};
}
} // namespace

OverrideHooks::FreeArchetypesFn OverrideHooks::s_freeArchetypes = nullptr;
OverrideHooks::ConstructArchetypesFn OverrideHooks::s_constructArchetypesOriginal = nullptr;
OverrideHooks::ModifyHierarchyStatusFn OverrideHooks::s_staticBoundsModifyHierarchyStatusOriginal =
    nullptr;
OverrideHooks::ModifyHierarchyStatusFn OverrideHooks::s_mapDataModifyHierarchyStatusOriginal =
    nullptr;
OverrideHooks::AddMapBoolEntryFn OverrideHooks::s_addMapBoolEntryOriginal = nullptr;
std::mutex OverrideHooks::s_overriddenMapIndexesMutex;
std::unordered_set<uint32_t> OverrideHooks::s_overriddenMapIndexes;

void OverrideHooks::Initialize(const GameAddresses& addresses)
{
    m_constructArchetypes = addresses.mapTypesConstructArchetypes;
    s_freeArchetypes = reinterpret_cast<FreeArchetypesFn>(addresses.archetypeManagerFreeArchetypes);
    m_staticBoundsModifyHierarchyStatus = addresses.staticBoundsStoreModifyHierarchyStatus;
    m_mapDataModifyHierarchyStatus = addresses.mapDataStoreModifyHierarchyStatus;
    m_addMapBoolEntryCall = addresses.packfileDependencyAddMapBoolEntryCall;
}

bool OverrideHooks::CanHookMapTypes() const
{
    return m_constructArchetypes != 0 && s_freeArchetypes != nullptr;
}

bool OverrideHooks::CanHookMapData() const
{
    return m_staticBoundsModifyHierarchyStatus != 0 && m_mapDataModifyHierarchyStatus != 0 &&
           m_addMapBoolEntryCall != 0;
}

Result<void> OverrideHooks::InstallMapTypesHooks()
{
    if (m_mapTypesInstalled)
    {
        return {};
    }
    if (!CanHookMapTypes())
    {
        return MakeError(ErrorCode::NotFound,
                         "the ConstructArchetypes and FreeArchetypes signatures did not resolve");
    }
    if (Result<void> ready = hooking::HookManager::Instance().Initialize(); !ready)
    {
        return ready;
    }
    if (Result<void> installed =
            InstallHook(kConstructArchetypesHook, m_constructArchetypes, &ConstructArchetypesDetour,
                        &s_constructArchetypesOriginal);
        !installed)
    {
        return installed;
    }
    m_mapTypesInstalled = true;
    return {};
}

Result<void> OverrideHooks::InstallMapDataHooks()
{
    if (m_mapDataInstalled)
    {
        return {};
    }
    if (!CanHookMapData())
    {
        return MakeError(ErrorCode::NotFound,
                         "the hierarchy status and packfile dependency signatures did not "
                         "resolve");
    }

    // The call is checked before anything is hooked, so a drifted signature changes nothing.
    const std::optional<uint8_t> opcode =
        SafeCall("OverrideHooks::ReadAddMapBoolEntryCall",
                 [&] { return *reinterpret_cast<const uint8_t*>(m_addMapBoolEntryCall); });
    if (!opcode || *opcode != kCallOpcode)
    {
        return MakeError(ErrorCode::NotFound,
                         "patch '{}' refused: the signature no longer points at a call",
                         kAddMapBoolEntryPatch);
    }
    const auto callee = reinterpret_cast<AddMapBoolEntryFn>(
        memory::Address(m_addMapBoolEntryCall).GetCallTarget().GetValue());

    if (Result<void> ready = hooking::HookManager::Instance().Initialize(); !ready)
    {
        return ready;
    }
    if (Result<void> installed = InstallHook(
            kStaticBoundsHierarchyHook, m_staticBoundsModifyHierarchyStatus,
            &StaticBoundsModifyHierarchyStatusDetour, &s_staticBoundsModifyHierarchyStatusOriginal);
        !installed)
    {
        return installed;
    }
    if (Result<void> installed = InstallHook(kMapDataHierarchyHook, m_mapDataModifyHierarchyStatus,
                                             &MapDataModifyHierarchyStatusDetour,
                                             &s_mapDataModifyHierarchyStatusOriginal);
        !installed)
    {
        return installed;
    }

    s_addMapBoolEntryOriginal = callee;
    Result<memory::CodePatch> redirected =
        memory::CodePatch::WriteCall(std::string{kAddMapBoolEntryPatch}, m_addMapBoolEntryCall,
                                     reinterpret_cast<void*>(&AddMapBoolEntryDetour));
    if (!redirected)
    {
        return redirected.GetError();
    }
    m_patches.Add(std::move(redirected.GetValue()));
    const memory::Module image = memory::Module::Main();
    SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {}+{:#x}", kAddMapBoolEntryPatch,
                  image.GetFileName(), m_addMapBoolEntryCall - image.GetBase());

    m_mapDataInstalled = true;
    return {};
}

void OverrideHooks::AddOverriddenMapIndex(GlobalIndex index)
{
    const std::scoped_lock lock(s_overriddenMapIndexesMutex);
    s_overriddenMapIndexes.insert(index.value);
}

void OverrideHooks::RemoveOverriddenMapIndex(GlobalIndex index)
{
    const std::scoped_lock lock(s_overriddenMapIndexesMutex);
    s_overriddenMapIndexes.erase(index.value);
}

void OverrideHooks::RestorePatches()
{
    m_patches.RestoreAll();
    m_mapDataInstalled = false; // the hierarchy hooks go with HookManager::Shutdown
}

bool OverrideHooks::IsOverriddenMapIndex(uint32_t globalIndex)
{
    const std::scoped_lock lock(s_overriddenMapIndexesMutex);
    return s_overriddenMapIndexes.contains(globalIndex);
}

int32_t OverrideHooks::KeepOverriddenActive(void* module, int32_t localSlot, int32_t status)
{
    if (status != HierarchyStatusLayout::kGameStatus || localSlot < 0)
    {
        return status;
    }
    const uint32_t baseIndex = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(module) + StreamingModuleLayout::kBaseIndex);
    if (!IsOverriddenMapIndex(baseIndex + static_cast<uint32_t>(localSlot)))
    {
        return status;
    }
    return HierarchyStatusLayout::kOverrideStatus;
}

void OverrideHooks::ConstructArchetypesDetour(void* mapTypes, int32_t localSlot)
{
    // "An asset won't get loaded without having been unloaded before", so freeing here only
    // ever frees what a previous copy of the same file left (FiveM LoadStreamingFile.cpp:3060).
    s_freeArchetypes(localSlot);
    s_constructArchetypesOriginal(mapTypes, localSlot);
}

bool OverrideHooks::StaticBoundsModifyHierarchyStatusDetour(void* module, int32_t localSlot,
                                                            int32_t status)
{
    return s_staticBoundsModifyHierarchyStatusOriginal(
        module, localSlot, KeepOverriddenActive(module, localSlot, status));
}

bool OverrideHooks::MapDataModifyHierarchyStatusDetour(void* module, int32_t localSlot,
                                                       int32_t status)
{
    return s_mapDataModifyHierarchyStatusOriginal(module, localSlot,
                                                  KeepOverriddenActive(module, localSlot, status));
}

void OverrideHooks::AddMapBoolEntryDetour(void* map, int32_t* globalIndex, bool* value)
{
    if (globalIndex != nullptr && *globalIndex >= 0 &&
        IsOverriddenMapIndex(static_cast<uint32_t>(*globalIndex)))
    {
        return; // our file must not become part of a DLC's dependencies
    }
    s_addMapBoolEntryOriginal(map, globalIndex, value);
}
} // namespace spl::rage
