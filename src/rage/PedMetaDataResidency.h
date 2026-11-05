#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "core/Result.h"
#include "rage/GameAddresses.h"
#include "rage/GameBuild.h"
#include "rage/StreamingInterface.h"
#include "rage/types/PedMetaDataTypes.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// The ped metadata limit. The streamer asks a
/// model for at most 100 dependencies and drops the rest, so a ped with many clothing-pack .ymt
/// files silently loses the ones past the limit. The hooks let the game list up to 256 and keep
/// what does not fit loaded by requesting it directly.
class PedMetaDataResidency
{
public:
    PedMetaDataResidency() = default;
    PedMetaDataResidency(const PedMetaDataResidency&) = delete;
    PedMetaDataResidency& operator=(const PedMetaDataResidency&) = delete;
    PedMetaDataResidency(PedMetaDataResidency&&) = delete;
    PedMetaDataResidency& operator=(PedMetaDataResidency&&) = delete;
    ~PedMetaDataResidency() = default;

    void Initialize(const GameAddresses& addresses, const GameBuild& build);

    /// Proves that the GetDependencies signature is the model info module's virtual before
    /// hooking it, then installs the hooks. The module array has to be set up, so this goes in
    /// once the bridge is verified. Without the RemovePedMetaDataFile signature everything else
    /// still goes in, and HasReleaseHook() is false. Idempotent.
    [[nodiscard]] Result<void> Install(const StreamingInterface& streaming);

    [[nodiscard]] bool IsInstalled() const
    {
        return m_installed;
    }

    /// True when a ped .ymt is released again once the game removes it from its last ped.
    [[nodiscard]] bool HasReleaseHook() const
    {
        return m_releaseHooked;
    }

private:
    using GetDependenciesFn = int (*)(void* self, uint32_t localSlot, uint32_t* outIndices,
                                      int count);
    using SetPedMetaDataFileFn = void (*)(void* self, int32_t localSlot);
    using RemovePedMetaDataFileFn = void (*)(void* self, int32_t localSlot);
    using RequestObjectFn = void (*)(void* manager, uint32_t index, int flags);
    using ClearRequiredFlagFn = void (*)(void* manager, uint32_t index, int flags);

    static int GetDependenciesDetour(void* self, uint32_t localSlot, uint32_t* outIndices,
                                     int count);
    static void SetPedMetaDataFileDetour(void* self, int32_t localSlot);
    static void RemovePedMetaDataFileDetour(void* self, int32_t localSlot);

    /// Records index as resident and requests it unless it still has its required flags.
    static void MakeResident(GlobalIndex index);

    /// Requests index again without recording anything.
    static void RequestResident(GlobalIndex index);

    /// Clears the flags RequestResident set, so the streamer may evict the file again.
    static void ReleaseResident(GlobalIndex index);

    /// The flags of an entry, or 0 when index is out of range.
    [[nodiscard]] static uint32_t ReadFlags(GlobalIndex index);

    /// Logs the first time a model has more dependencies than the game asked for.
    static void ReportOverflow(uint32_t localSlot, int total);

    /// Exactly one streaming module must have the GetDependencies address in its virtual slot.
    [[nodiscard]] Result<void> VerifyDependenciesSlot() const;

    uintptr_t m_manager = 0;
    uintptr_t m_getDependencies = 0;
    uintptr_t m_setPedMetaDataFile = 0;
    uintptr_t m_removePedMetaDataFile = 0;
    uintptr_t m_requestObject = 0;
    uintptr_t m_clearRequiredFlag = 0;
    size_t m_vtableShift = 0;
    bool m_installed = false;
    bool m_releaseHooked = false;

    // The detours run inside game code and have no object to reach, so their state is static.
    static const strStreamingInfoManagerView* s_manager;
    static uint32_t s_metaDataBaseIndex;
    static RequestObjectFn s_requestObject;
    static ClearRequiredFlagFn s_clearRequiredFlag;
    static GetDependenciesFn s_getDependenciesOriginal;
    static SetPedMetaDataFileFn s_setPedMetaDataFileOriginal;
    static RemovePedMetaDataFileFn s_removePedMetaDataFileOriginal;

    // Guarded by s_mutex. Nothing calls into the streamer while holding it, because a request
    // can come straight back into GetDependenciesDetour.
    static std::mutex s_mutex;
    static std::unordered_set<uint32_t> s_residentIndices;
    static PedMetaOwners s_owners;
    static std::unordered_set<uint32_t> s_overflowSlots;
    static std::atomic<bool> s_warnedAtLimit;
};
} // namespace spl::rage
