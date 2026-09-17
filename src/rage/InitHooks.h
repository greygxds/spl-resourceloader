#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"
#include "memory/CodePatch.h"
#include "rage/GameAddresses.h"

namespace spl::rage
{
/// The phases rage::gameSkeleton runs its init functions in (FiveM gta-core-five
/// include/gameSkeleton.h).
enum class InitPhase : int32_t
{
    Core = 1,
    BeforeMapLoaded = 2,
    AfterMapLoaded = 4,
    Session = 8
};

[[nodiscard]] std::string_view ToString(InitPhase phase);

/// What the game's startup reports to the loader. Every callback runs on the game's main
/// thread, inside game code, so none of them may block for long.
struct InitHookCallbacks
{
    std::function<void()> onInitialMount; ///< the game's devices are mounted, no data read yet
    std::function<void(InitPhase)> onPhaseStart;
    std::function<void(InitPhase)> onPhaseEnd;
};

/// Level metas, as VFS paths, handed to the game's level loader.
struct LevelMetas
{
    std::vector<std::string> init;   ///< loaded when the map starts loading (init_meta)
    std::vector<std::string> before; ///< loaded just before the level's own list
    std::vector<std::string> after;  ///< loaded just after it
};

/// The hooks that let the loader work while the game starts, as FiveM does, instead
/// of waiting for story mode. Installed once, before the game's code first runs its init.
class InitHooks
{
public:
    InitHooks() = default;
    InitHooks(const InitHooks&) = delete;
    InitHooks& operator=(const InitHooks&) = delete;

    /// True once the startup hook's signature matches in the running image, which it does not
    /// while GTA5.exe's code is still encrypted. Scans quietly, so it can be asked repeatedly.
    [[nodiscard]] static bool IsGameCodeReady();

    /// Hooks RunInitFunctions, which is required. The initial mount and the level metas are
    /// optional: a miss is logged and only that part is missing.
    [[nodiscard]] Result<void> Install(const GameAddresses& addresses, InitHookCallbacks callbacks);

    [[nodiscard]] bool HasInitialMountHook() const
    {
        return m_initialMountHooked;
    }

    [[nodiscard]] bool HasLevelMetaHooks() const
    {
        return m_levelMetasHooked;
    }

    /// Makes the game enable GROUP_MAP instead of GROUP_MAP_SP when it sets up its map layer, so
    /// story mode starts with GTA Online's maps as FiveM does. Must go in before the game runs
    /// its init, and is checked against the original bytes first.
    [[nodiscard]] Result<void> InstallMultiplayerMapsPatch(const GameAddresses& addresses);

    /// Set once the resources are mounted; read when the game gets to its level load.
    void SetLevelMetas(LevelMetas metas);

    /// Puts the redirected calls back, which point into this module. The MinHook hook is
    /// HookManager's to remove.
    void RestorePatches()
    {
        m_patches.RestoreAll();
    }

private:
    using RunInitFunctionsFn = void (*)(void* skeleton, int32_t phase);
    using InitialMountFn = void (*)();
    using LoadDatFn = void (*)(void* dataFileMgr, const char* name, bool enabled);

    static void RunInitFunctionsDetour(void* skeleton, int32_t phase);
    static void InitialMountDetour();
    static void LoadLevelDatDetour(void* dataFileMgr, const char* name, bool enabled);
    static void LoadDefDatDetour(void* dataFileMgr, const char* name, bool enabled);

    /// Loads each meta through the game's LoadDat, logging the ones it faults on.
    static void LoadMetas(void* dataFileMgr, const std::vector<std::string>& metas, bool enabled,
                          std::string_view kind);

    /// FiveM's device patches: keep the update:/ devices from being sorted in front of ours, and
    /// raise the mount limit. Both optional.
    void InstallMountPatches(const GameAddresses& addresses);

    [[nodiscard]] Result<void> RedirectCall(std::string_view name, uintptr_t call, void* detour,
                                            uintptr_t& callee);

    bool m_initialMountHooked = false;
    bool m_levelMetasHooked = false;
    memory::PatchRegistry m_patches;

    // The detours run inside game code and have no object to reach, so their state is static.
    static InitHookCallbacks s_callbacks;
    static LevelMetas s_levelMetas;
    static RunInitFunctionsFn s_runInitFunctionsOriginal;
    static InitialMountFn s_initialMountOriginal;
    static LoadDatFn s_loadDat;
    static LoadDatFn s_loadDefDat;
    static void* s_dataFileMgr;
};
} // namespace spl::rage
