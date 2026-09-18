#include "rage/InitHooks.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "hooking/HookManager.h"
#include "logging/Logger.h"
#include "memory/Address.h"
#include "memory/Module.h"
#include "memory/Pattern.h"
#include "memory/PatternScanner.h"
#include "rage/SafeCall.h"
#include "rage/signatures/SignatureSpec.h"
#include "rage/types/MapStoreTypes.h"
#include "util/Hash.h"

namespace spl::rage
{
namespace
{
constexpr std::string_view kRunInitFunctionsHook = "rage::gameSkeleton::RunInitFunctions";
constexpr std::string_view kInitialMountPatch = "rage::fiDevice::InitialMountCall";
constexpr std::string_view kLoadDatPatch = "CDataFileMgr::LoadDatCall";
constexpr std::string_view kLoadDefDatPatch = "CDataFileMgr::LoadDefDatCall";

constexpr std::string_view kSortRelativeDevicesPatch = "rage::fiDevice::SortRelativeDevicesCall";
constexpr std::string_view kMountLimitPatch = "rage::fiDevice::MountLimit";
constexpr std::string_view kStartupMapGroupPatch = "StartupMapGroup";

constexpr uint8_t kCallOpcode = 0xE8;
constexpr std::size_t kCallLengthBytes = 5;

/// FiveM multiplies the limit by 15 (HookInitialMount.cpp:217); mods add four mounts each.
constexpr uint32_t kVanillaMountLimit = 100;
constexpr uint32_t kRaisedMountLimit = kVanillaMountLimit * 15;

/// An imm32 as the bytes it is stored as.
[[nodiscard]] std::array<uint8_t, 4> LittleEndianBytes(uint32_t value)
{
    return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
}

[[nodiscard]] std::string DescribeAddress(uintptr_t address)
{
    const memory::Module image = memory::Module::Main();
    return image.Contains(address)
               ? fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase())
               : fmt::format("{:#x}", address);
}

/// Who owns the current target of a startup call: nullopt when it is still the game's own
/// code (or there is no call to check), so the loader may redirect it. A hook trampoline
/// lives outside any loaded image, which still counts as owned by another mod.
[[nodiscard]] std::optional<std::string> FindForeignCallOwner(uintptr_t call)
{
    if (call == 0)
    {
        return std::nullopt;
    }
    const std::optional<uintptr_t> callee =
        SafeCall("InitHooks::ReadCallTarget",
                 [&] { return memory::Address(call).GetCallTarget().GetValue(); });
    if (!callee)
    {
        return std::string{"an unreadable address"};
    }
    const std::optional<memory::Module> owner = memory::Module::FindContaining(*callee);
    if (owner && owner->GetBase() == memory::Module::Main().GetBase())
    {
        return std::nullopt;
    }
    if (owner)
    {
        return owner->GetFileName();
    }
    return std::string{"a hook trampoline outside any loaded module"};
}
} // namespace

InitHookCallbacks InitHooks::s_callbacks;
LevelMetas InitHooks::s_levelMetas;
InitHooks::RunInitFunctionsFn InitHooks::s_runInitFunctionsOriginal = nullptr;
InitHooks::InitialMountFn InitHooks::s_initialMountOriginal = nullptr;
InitHooks::LoadDatFn InitHooks::s_loadDat = nullptr;
InitHooks::LoadDatFn InitHooks::s_loadDefDat = nullptr;
void* InitHooks::s_dataFileMgr = nullptr;

bool InitHooks::IsGameCodeReady()
{
    const std::span<const SignatureSpec> signatures = AllSignatures();
    const auto spec = std::ranges::find(signatures, kRunInitFunctionsHook, &SignatureSpec::name);
    if (spec == signatures.end())
    {
        return false;
    }
    const std::optional<memory::Pattern> pattern = memory::Pattern::Parse(spec->pattern);
    if (!pattern)
    {
        return false;
    }
    memory::PatternScanner scanner{memory::Module::Main()};
    return !scanner.Scan(*pattern, 2).empty();
}

std::string_view ToString(InitPhase phase)
{
    switch (phase)
    {
        using enum InitPhase;
    case Core:
        return "INIT_CORE";
    case BeforeMapLoaded:
        return "INIT_BEFORE_MAP_LOADED";
    case AfterMapLoaded:
        return "INIT_AFTER_MAP_LOADED";
    case Session:
        return "INIT_SESSION";
    }
    return "INIT_UNKNOWN";
}

Result<void> InitHooks::Install(const GameAddresses& addresses, InitHookCallbacks callbacks)
{
    if (addresses.gameSkeletonRunInitFunctions == 0)
    {
        return MakeError(ErrorCode::NotFound, "signature '{}' did not resolve",
                         kRunInitFunctionsHook);
    }
    s_callbacks = std::move(callbacks);

    hooking::HookManager& hooks = hooking::HookManager::Instance();
    if (Result<void> ready = hooks.Initialize(); !ready)
    {
        return ready;
    }

    // The optional call sites go in first: once RunInitFunctions is hooked the game may run
    // at any moment, and a half-installed set would miss its events. Device setup is one
    // unit: when another mod already owns the initial mount call, it owns ordering and the
    // mount limit too, and our writes there would only corrupt its flow.
    const std::optional<std::string> deviceSetupOwner =
        FindForeignCallOwner(addresses.fiDeviceInitialMountCall);
    if (deviceSetupOwner)
    {
        SPL_LOG_WARNING(Hook,
                        "Device setup stays with '{}', which already hooked the initial mount: "
                        "mount limit and device order are unchanged",
                        *deviceSetupOwner);
    }
    else
    {
        InstallMountPatches(addresses);
    }
    if (addresses.fiDeviceInitialMountCall != 0 && !deviceSetupOwner)
    {
        uintptr_t callee = 0;
        if (Result<void> redirected =
                RedirectCall(kInitialMountPatch, addresses.fiDeviceInitialMountCall,
                             reinterpret_cast<void*>(&InitialMountDetour), callee);
            !redirected)
        {
            SPL_LOG_WARNING(Hook, "Mods cannot replace game files before the game reads them: {}",
                            redirected.GetMessage());
        }
        else
        {
            s_initialMountOriginal = reinterpret_cast<InitialMountFn>(callee);
            m_initialMountHooked = true;
        }
    }

    if (addresses.dataFileMgrLoadDatCall != 0 && addresses.dataFileMgrLoadDefDatCall != 0)
    {
        uintptr_t loadDat = 0;
        uintptr_t loadDefDat = 0;
        Result<void> redirected =
            RedirectCall(kLoadDatPatch, addresses.dataFileMgrLoadDatCall,
                         reinterpret_cast<void*>(&LoadLevelDatDetour), loadDat);
        if (redirected)
        {
            s_loadDat = reinterpret_cast<LoadDatFn>(loadDat); // the patched call needs it now
            redirected = RedirectCall(kLoadDefDatPatch, addresses.dataFileMgrLoadDefDatCall,
                                      reinterpret_cast<void*>(&LoadDefDatDetour), loadDefDat);
        }
        if (!redirected)
        {
            SPL_LOG_WARNING(Hook, "Level metas cannot be loaded: {}", redirected.GetMessage());
        }
        else
        {
            s_loadDefDat = reinterpret_cast<LoadDatFn>(loadDefDat);
            m_levelMetasHooked = true;
        }
    }

    if (Result<void> created =
            hooks.Create(kRunInitFunctionsHook, addresses.gameSkeletonRunInitFunctions,
                         &RunInitFunctionsDetour, &s_runInitFunctionsOriginal);
        !created)
    {
        return created;
    }
    if (Result<void> enabled = hooks.Enable(kRunInitFunctionsHook); !enabled)
    {
        return enabled;
    }
    SPL_LOG_DEBUG(Hook, "Hook '{}' installed at {}", kRunInitFunctionsHook,
                  DescribeAddress(addresses.gameSkeletonRunInitFunctions));
    return {};
}

void InitHooks::InstallMountPatches(const GameAddresses& addresses)
{
    // Without this the game sorts its update:/ devices in front of the mods' overlays once it
    // mounts update2, and serves its own files again.
    if (addresses.fiDeviceSortRelativeDevicesCall != 0)
    {
        const std::optional<uint8_t> opcode = SafeCall(
            "InitHooks::ReadCall",
            [&]
            {
                return *reinterpret_cast<const uint8_t*>(addresses.fiDeviceSortRelativeDevicesCall);
            });
        if (!opcode || *opcode != kCallOpcode)
        {
            SPL_LOG_WARNING(Hook, "Patch '{}' refused: the signature is not a call",
                            kSortRelativeDevicesPatch);
        }
        else if (Result<memory::CodePatch> patch = memory::CodePatch::Nop(
                     std::string{kSortRelativeDevicesPatch},
                     addresses.fiDeviceSortRelativeDevicesCall, kCallLengthBytes);
                 !patch)
        {
            SPL_LOG_WARNING(Hook, "The game may put its own files back in front of mod files: {}",
                            patch.GetMessage());
        }
        else
        {
            m_patches.Add(std::move(patch.GetValue()));
            SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {}", kSortRelativeDevicesPatch,
                          DescribeAddress(addresses.fiDeviceSortRelativeDevicesCall));
        }
    }

    if (addresses.fiDeviceMountLimit != 0)
    {
        const std::array<uint8_t, 4> expected = LittleEndianBytes(kVanillaMountLimit);
        const std::array<uint8_t, 4> raised = LittleEndianBytes(kRaisedMountLimit);
        if (Result<void> applied = m_patches.Apply(std::string{kMountLimitPatch},
                                                   addresses.fiDeviceMountLimit, raised, expected);
            !applied)
        {
            SPL_LOG_WARNING(Hook, "The game's mount limit was not raised: {}",
                            applied.GetMessage());
        }
        else
        {
            SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {} ({} mounts)", kMountLimitPatch,
                          DescribeAddress(addresses.fiDeviceMountLimit), kRaisedMountLimit);
        }
    }
}

Result<void> InitHooks::InstallMultiplayerMapsPatch(const GameAddresses& addresses)
{
    if (addresses.startupMapGroup == 0)
    {
        return MakeError(ErrorCode::NotFound, "signature '{}' did not resolve",
                         kStartupMapGroupPatch);
    }
    const std::array<uint8_t, 4> story =
        LittleEndianBytes(util::JoaatLower(ContentGroupLayout::kStoryMapGroup));
    const std::array<uint8_t, 4> multiplayer =
        LittleEndianBytes(util::JoaatLower(ContentGroupLayout::kMultiplayerMapGroup));
    if (Result<void> applied = m_patches.Apply(std::string{kStartupMapGroupPatch},
                                               addresses.startupMapGroup, multiplayer, story);
        !applied)
    {
        return applied;
    }
    SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {} ({} -> {})", kStartupMapGroupPatch,
                  DescribeAddress(addresses.startupMapGroup), ContentGroupLayout::kStoryMapGroup,
                  ContentGroupLayout::kMultiplayerMapGroup);
    return {};
}

void InitHooks::SetLevelMetas(LevelMetas metas)
{
    s_levelMetas = std::move(metas);
}

Result<void> InitHooks::RedirectCall(std::string_view name, uintptr_t call, void* detour,
                                     uintptr_t& callee)
{
    const std::optional<uint8_t> opcode =
        SafeCall("InitHooks::ReadCall", [&] { return *reinterpret_cast<const uint8_t*>(call); });
    if (!opcode || *opcode != kCallOpcode)
    {
        return MakeError(ErrorCode::NotFound, "patch '{}' refused: the signature is not a call",
                         name);
    }
    callee = memory::Address(call).GetCallTarget().GetValue();
    // Another mod may have redirected the call first (Lenny's Mod Loader hooks the same
    // startup calls). Chaining into a foreign detour runs unknown code as the game's own
    // startup, which froze the game with no window; refuse instead and degrade to late hooks.
    if (const std::optional<std::string> foreign = FindForeignCallOwner(call))
    {
        return MakeError(ErrorCode::NotFound,
                         "patch '{}' refused: the call already points into '{}', so another mod "
                         "hooked it first",
                         name, *foreign);
    }
    Result<memory::CodePatch> patch = memory::CodePatch::WriteCall(std::string{name}, call, detour);
    if (!patch)
    {
        return patch.GetError();
    }
    m_patches.Add(std::move(patch.GetValue()));
    SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {}", name, DescribeAddress(call));
    return {};
}

void InitHooks::RunInitFunctionsDetour(void* skeleton, int32_t phase)
{
    const auto initPhase = static_cast<InitPhase>(phase);
    SPL_LOG_DEBUG(Hook, "{} starts", ToString(initPhase));

    // FiveM loads init_meta files as the map starts loading (LoadStreamingFile.cpp:3652).
    if (initPhase == InitPhase::BeforeMapLoaded && s_dataFileMgr != nullptr)
    {
        LoadMetas(s_dataFileMgr, s_levelMetas.init, true, "init_meta");
    }
    if (s_callbacks.onPhaseStart)
    {
        s_callbacks.onPhaseStart(initPhase);
    }

    s_runInitFunctionsOriginal(skeleton, phase);

    if (s_callbacks.onPhaseEnd)
    {
        s_callbacks.onPhaseEnd(initPhase);
    }
    SPL_LOG_DEBUG(Hook, "{} ends", ToString(initPhase));
}

void InitHooks::InitialMountDetour()
{
    s_initialMountOriginal();
    if (s_callbacks.onInitialMount)
    {
        s_callbacks.onInitialMount();
    }
}

void InitHooks::LoadLevelDatDetour(void* dataFileMgr, const char* name, bool enabled)
{
    LoadMetas(dataFileMgr, s_levelMetas.before, enabled, "before_level_meta");
    s_loadDat(dataFileMgr, name, enabled);
    LoadMetas(dataFileMgr, s_levelMetas.after, enabled, "after_level_meta");
}

void InitHooks::LoadDefDatDetour(void* dataFileMgr, const char* name, bool enabled)
{
    s_dataFileMgr = dataFileMgr;
    s_loadDefDat(dataFileMgr, name, enabled);
}

void InitHooks::LoadMetas(void* dataFileMgr, const std::vector<std::string>& metas, bool enabled,
                          std::string_view kind)
{
    for (const std::string& meta : metas)
    {
        const bool loaded = SafeCall("CDataFileMgr::LoadDat",
                                     [&] { s_loadDat(dataFileMgr, meta.c_str(), enabled); });
        if (loaded)
        {
            SPL_LOG_INFO(Streaming, "Loaded {} '{}'", kind, meta);
        }
    }
}
} // namespace spl::rage
