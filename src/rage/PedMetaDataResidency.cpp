#include "rage/PedMetaDataResidency.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>

#include "hooking/HookManager.h"
#include "logging/Logger.h"
#include "memory/Module.h"
#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/VirtualCall.h"

namespace spl::rage
{
namespace
{
constexpr std::string_view kGetDependenciesHook = "CModelInfoStreamingModule::GetDependencies";
constexpr std::string_view kSetPedMetaDataFileHook = "CPedModelInfo::SetPedMetaDataFile";
constexpr std::string_view kRemovePedMetaDataFileHook = "CPedModelInfo::RemovePedMetaDataFile";

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

const strStreamingInfoManagerView* PedMetaDataResidency::s_manager = nullptr;
uint32_t PedMetaDataResidency::s_metaDataBaseIndex = 0;
PedMetaDataResidency::RequestObjectFn PedMetaDataResidency::s_requestObject = nullptr;
PedMetaDataResidency::ClearRequiredFlagFn PedMetaDataResidency::s_clearRequiredFlag = nullptr;
PedMetaDataResidency::GetDependenciesFn PedMetaDataResidency::s_getDependenciesOriginal = nullptr;
PedMetaDataResidency::SetPedMetaDataFileFn PedMetaDataResidency::s_setPedMetaDataFileOriginal =
    nullptr;
PedMetaDataResidency::RemovePedMetaDataFileFn
    PedMetaDataResidency::s_removePedMetaDataFileOriginal = nullptr;
std::mutex PedMetaDataResidency::s_mutex;
std::unordered_set<uint32_t> PedMetaDataResidency::s_residentIndices;
PedMetaOwners PedMetaDataResidency::s_owners;
std::unordered_set<uint32_t> PedMetaDataResidency::s_overflowSlots;
std::atomic<bool> PedMetaDataResidency::s_warnedAtLimit{false};

void PedMetaDataResidency::Initialize(const GameAddresses& addresses, const GameBuild& build)
{
    m_manager = addresses.streamingInfoManagerInstance;
    m_getDependencies = addresses.modelInfoStreamingModuleGetDependencies;
    m_setPedMetaDataFile = addresses.pedModelInfoSetPedMetaDataFile;
    m_removePedMetaDataFile = addresses.pedModelInfoRemovePedMetaDataFile;
    m_requestObject = addresses.streamingInfoManagerRequestObject;
    m_clearRequiredFlag = addresses.streamingInfoManagerClearRequiredFlag;
    m_vtableShift = StreamingModuleLayout::VtableShift(build.build);
}

Result<void> PedMetaDataResidency::Install(const StreamingInterface& streaming)
{
    if (m_installed)
    {
        return {};
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted; no hook is installed");
    }
    if (m_manager == 0 || m_getDependencies == 0 || m_setPedMetaDataFile == 0 ||
        m_requestObject == 0 || m_clearRequiredFlag == 0)
    {
        return MakeError(ErrorCode::NotFound,
                         "the ped metadata signatures did not resolve on this build");
    }
    const std::optional<StreamingModule> metaData = streaming.GetModule("ymt");
    if (!metaData)
    {
        return MakeError(ErrorCode::NotFound, "the game has no 'ymt' streaming module");
    }
    if (Result<void> verified = VerifyDependenciesSlot(); !verified)
    {
        return verified;
    }

    // Everything the detours read is set before the first one can run.
    s_manager = reinterpret_cast<const strStreamingInfoManagerView*>(m_manager);
    s_metaDataBaseIndex = metaData->BaseIndex();
    s_requestObject = reinterpret_cast<RequestObjectFn>(m_requestObject);
    s_clearRequiredFlag = reinterpret_cast<ClearRequiredFlagFn>(m_clearRequiredFlag);

    if (Result<void> ready = hooking::HookManager::Instance().Initialize(); !ready)
    {
        return ready;
    }
    if (Result<void> installed = InstallHook(kGetDependenciesHook, m_getDependencies,
                                             &GetDependenciesDetour, &s_getDependenciesOriginal);
        !installed)
    {
        return installed;
    }
    if (Result<void> installed =
            InstallHook(kSetPedMetaDataFileHook, m_setPedMetaDataFile, &SetPedMetaDataFileDetour,
                        &s_setPedMetaDataFileOriginal);
        !installed)
    {
        return installed;
    }
    m_installed = true;

    // Without it files stay loaded for good, which wastes memory and breaks nothing.
    if (m_removePedMetaDataFile != 0)
    {
        if (Result<void> installed =
                InstallHook(kRemovePedMetaDataFileHook, m_removePedMetaDataFile,
                            &RemovePedMetaDataFileDetour, &s_removePedMetaDataFileOriginal);
            installed)
        {
            m_releaseHooked = true;
        }
        else
        {
            SPL_LOG_DEBUG(Hook, "Hook '{}' was not installed: {}", kRemovePedMetaDataFileHook,
                          installed.GetMessage());
        }
    }
    SPL_LOG_DEBUG(Hook, "Ped metadata limit raised from {} to {} dependencies (release hook {})",
                  PedMetaDataLayout::kVanillaMaxDependencies,
                  PedMetaDataLayout::kExtendedMaxDependencies, m_releaseHooked ? "on" : "off");
    return {};
}

Result<void> PedMetaDataResidency::VerifyDependenciesSlot() const
{
    const auto* const manager = reinterpret_cast<const strStreamingInfoManagerView*>(m_manager);
    const atArrayView<void*>& modules = manager->moduleMgr.modules;
    if (modules.data == nullptr || modules.count == 0 ||
        !IsReadableMemory(reinterpret_cast<uintptr_t>(modules.data), modules.count * sizeof(void*)))
    {
        return MakeError(ErrorCode::Unavailable, "the streaming modules are not set up yet");
    }

    const std::size_t slot = StreamingModuleLayout::kSlotGetDependencies + m_vtableShift;
    std::size_t matches = 0;
    for (uint16_t index = 0; index < modules.count; ++index)
    {
        const auto module = reinterpret_cast<uintptr_t>(modules.data[index]);
        if (module == 0 || !IsReadableMemory(module, sizeof(uintptr_t)))
        {
            continue;
        }
        const uintptr_t vtable = GetVtableAddress(module);
        if (!IsReadableMemory(vtable, (slot + 1) * sizeof(uintptr_t)))
        {
            continue;
        }
        if (reinterpret_cast<const uintptr_t*>(vtable)[slot] == m_getDependencies)
        {
            ++matches;
        }
    }
    if (matches == 0)
    {
        return MakeError(ErrorCode::NotFound,
                         "no streaming module has {:#x} in its GetDependencies slot, so the "
                         "signature no longer points at the model info module's",
                         m_getDependencies);
    }
    if (matches > 1)
    {
        return MakeError(ErrorCode::Ambiguous,
                         "{} streaming modules have {:#x} in their GetDependencies slot", matches,
                         m_getDependencies);
    }
    return {};
}

uint32_t PedMetaDataResidency::ReadFlags(GlobalIndex index)
{
    const strStreamingInfoManagerView* const manager = s_manager;
    if (manager == nullptr || manager->entries == nullptr || manager->numEntries < 0 ||
        index.value >= static_cast<uint32_t>(manager->numEntries))
    {
        return 0;
    }
    return manager->entries[index.value].flags;
}

void PedMetaDataResidency::RequestResident(GlobalIndex index)
{
    const strStreamingInfoManagerView* const manager = s_manager;
    if (manager == nullptr || manager->entries == nullptr || manager->numEntries < 0 ||
        index.value >= static_cast<uint32_t>(manager->numEntries) || HasFaulted())
    {
        return;
    }
    // A fault is logged by SafeCall and stops every later game call, so nothing more to do here.
    void* const target = const_cast<strStreamingInfoManagerView*>(manager);
    static_cast<void>(SafeCall(
        "strStreamingInfoManager::RequestObject",
        [&] { s_requestObject(target, index.value, ToInt(PedMetaDataLayout::kResidentRequest)); }));
}

void PedMetaDataResidency::MakeResident(GlobalIndex index)
{
    const strStreamingInfoManagerView* const manager = s_manager;
    if (manager == nullptr || manager->entries == nullptr || manager->numEntries < 0 ||
        index.value >= static_cast<uint32_t>(manager->numEntries))
    {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        s_residentIndices.insert(index.value);
    }
    if (!HasRequiredFlags(ReadFlags(index)))
    {
        RequestResident(index);
    }
}

void PedMetaDataResidency::ReleaseResident(GlobalIndex index)
{
    const strStreamingInfoManagerView* const manager = s_manager;
    if (manager == nullptr || manager->entries == nullptr || manager->numEntries < 0 ||
        index.value >= static_cast<uint32_t>(manager->numEntries) || HasFaulted())
    {
        return;
    }
    void* const target = const_cast<strStreamingInfoManagerView*>(manager);
    static_cast<void>(SafeCall(
        "strStreamingInfoManager::ClearRequiredFlag",
        [&] { s_clearRequiredFlag(target, index.value, ToInt(PedMetaDataLayout::kRelease)); }));
}

void PedMetaDataResidency::ReportOverflow(uint32_t localSlot, int total)
{
    bool firstTime = false;
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        firstTime = s_overflowSlots.insert(localSlot).second;
    }
    if (firstTime)
    {
        SPL_LOG_DEBUG(Rage,
                      "Model info slot {} has {} dependencies, more than the game asks for; the "
                      "rest are kept loaded instead",
                      localSlot, total);
    }
    if (total >= PedMetaDataLayout::kExtendedMaxDependencies && !s_warnedAtLimit.exchange(true))
    {
        SPL_LOG_WARNING(Rage,
                        "A model has {} or more dependencies, which is more than the extended "
                        "limit holds; the ones past it cannot load",
                        PedMetaDataLayout::kExtendedMaxDependencies);
    }
}

int PedMetaDataResidency::GetDependenciesDetour(void* self, uint32_t localSlot,
                                                uint32_t* outIndices, int count)
{
    const strStreamingInfoManagerView* const manager = s_manager;
    if (count <= 0 || count >= PedMetaDataLayout::kExtendedMaxDependencies ||
        outIndices == nullptr || manager == nullptr || manager->entries == nullptr || HasFaulted())
    {
        return s_getDependenciesOriginal(self, localSlot, outIndices, count);
    }

    std::array<uint32_t, PedMetaDataLayout::kExtendedMaxDependencies> dependencies{};
    constexpr int kLimit = PedMetaDataLayout::kExtendedMaxDependencies;
    const int total = std::clamp(
        s_getDependenciesOriginal(self, localSlot, dependencies.data(), kLimit), 0, kLimit);

    std::array<uint32_t, PedMetaDataLayout::kExtendedMaxDependencies> rearm{};
    std::unique_lock<std::mutex> lock(s_mutex, std::defer_lock);
    if (total > count)
    {
        lock.lock(); // a list that fits never reads the resident set
    }
    const DependencySplit split = SplitDependencies(
        std::span<const uint32_t>(dependencies.data(), static_cast<std::size_t>(total)), count,
        std::span<uint32_t>(outIndices, static_cast<std::size_t>(count)), rearm,
        [](uint32_t index) { return s_residentIndices.contains(index); },
        [](uint32_t index) { return ReadFlags(GlobalIndex{index}); });
    if (lock.owns_lock())
    {
        lock.unlock();
    }

    for (int index = 0; index < split.rearmCount; ++index)
    {
        RequestResident(GlobalIndex{rearm[static_cast<std::size_t>(index)]});
    }
    for (int index = split.overflowBegin; index < total; ++index)
    {
        MakeResident(GlobalIndex{dependencies[static_cast<std::size_t>(index)]});
    }
    if (total > count)
    {
        ReportOverflow(localSlot, total);
    }
    return split.written;
}

void PedMetaDataResidency::SetPedMetaDataFileDetour(void* self, int32_t localSlot)
{
    s_setPedMetaDataFileOriginal(self, localSlot);

    const std::optional<GlobalIndex> index = ToGlobalIndex(localSlot, s_metaDataBaseIndex);
    if (!index || HasFaulted())
    {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        s_owners.AddOwner(*index, reinterpret_cast<uintptr_t>(self));
    }
    MakeResident(*index);
}

void PedMetaDataResidency::RemovePedMetaDataFileDetour(void* self, int32_t localSlot)
{
    s_removePedMetaDataFileOriginal(self, localSlot);

    const std::optional<GlobalIndex> index = ToGlobalIndex(localSlot, s_metaDataBaseIndex);
    if (!index || HasFaulted())
    {
        return;
    }
    // Only a file this class made resident is released, and only once no ped is left holding it.
    bool release = false;
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        if (s_owners.RemoveOwner(*index, reinterpret_cast<uintptr_t>(self)))
        {
            release = s_residentIndices.erase(index->value) > 0;
        }
    }
    if (release)
    {
        ReleaseResident(*index);
        SPL_LOG_DEBUG(Rage, "Ped metadata slot {} released", localSlot);
    }
}
} // namespace spl::rage
