#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "core/Result.h"
#include "memory/CodePatch.h"
#include "rage/GameAddresses.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// The hooks overriding a game .ytyp, .ymap or .ybn needs. Like GamePatches,
/// nothing is installed until an override of that kind is registered, so a setup without
/// such overrides never runs any of this code.
class OverrideHooks
{
public:
    OverrideHooks() = default;
    OverrideHooks(const OverrideHooks&) = delete;
    OverrideHooks& operator=(const OverrideHooks&) = delete;
    OverrideHooks(OverrideHooks&&) = delete;
    OverrideHooks& operator=(OverrideHooks&&) = delete;
    ~OverrideHooks() = default;

    void Initialize(const GameAddresses& addresses);

    /// True when both map-types signatures resolved.
    [[nodiscard]] bool CanHookMapTypes() const;

    /// True when every map-data signature resolved.
    [[nodiscard]] bool CanHookMapData() const;

    /// fwMapTypes::ConstructArchetypes frees the archetypes of the file it rebuilds first, so a
    /// game .ytyp replaced by one with a different archetype count does not leave stale ones
    /// behind (FiveM LoadStreamingFile.cpp:3056). Idempotent.
    [[nodiscard]] Result<void> InstallMapTypesHooks();

    /// Keeps overridden map data and static bounds active when the game changes their hierarchy
    /// status, and keeps them out of DLC packfile dependency maps (FiveM
    /// LoadStreamingFile.cpp:3007, 2910). Idempotent.
    [[nodiscard]] Result<void> InstallMapDataHooks();

    /// Marks a .ymap or .ybn slot as one of our overrides for the map-data hooks.
    void AddOverriddenMapIndex(GlobalIndex index);

    /// Takes a slot out again.
    void RemoveOverriddenMapIndex(GlobalIndex index);

    /// Puts the redirected call back. The MinHook hooks go with HookManager::Shutdown.
    void RestorePatches();

private:
    using ConstructArchetypesFn = void (*)(void* mapTypes, int32_t localSlot);
    using FreeArchetypesFn = void (*)(int32_t localSlot);
    using ModifyHierarchyStatusFn = bool (*)(void* module, int32_t localSlot, int32_t status);
    using AddMapBoolEntryFn = void (*)(void* map, int32_t* globalIndex, bool* value);

    static void ConstructArchetypesDetour(void* mapTypes, int32_t localSlot);
    static bool StaticBoundsModifyHierarchyStatusDetour(void* module, int32_t localSlot,
                                                        int32_t status);
    static bool MapDataModifyHierarchyStatusDetour(void* module, int32_t localSlot, int32_t status);
    static void AddMapBoolEntryDetour(void* map, int32_t* globalIndex, bool* value);

    /// The status the game passes, with the one FiveM swaps in for our own slots.
    [[nodiscard]] static int32_t KeepOverriddenActive(void* module, int32_t localSlot,
                                                      int32_t status);
    [[nodiscard]] static bool IsOverriddenMapIndex(uint32_t globalIndex);

    uintptr_t m_constructArchetypes = 0;
    uintptr_t m_staticBoundsModifyHierarchyStatus = 0;
    uintptr_t m_mapDataModifyHierarchyStatus = 0;
    uintptr_t m_addMapBoolEntryCall = 0;
    bool m_mapTypesInstalled = false;
    bool m_mapDataInstalled = false;
    memory::PatchRegistry m_patches;

    // The detours run inside game code and have no object to reach, so their state is static.
    static FreeArchetypesFn s_freeArchetypes;
    static ConstructArchetypesFn s_constructArchetypesOriginal;
    static ModifyHierarchyStatusFn s_staticBoundsModifyHierarchyStatusOriginal;
    static ModifyHierarchyStatusFn s_mapDataModifyHierarchyStatusOriginal;
    static AddMapBoolEntryFn s_addMapBoolEntryOriginal;
    static std::mutex s_overriddenMapIndexesMutex;
    static std::unordered_set<uint32_t> s_overriddenMapIndexes;
};
} // namespace spl::rage
