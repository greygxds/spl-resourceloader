#pragma once

#include <cstdint>

#include "core/Result.h"
#include "memory/CodePatch.h"
#include "rage/GameAddresses.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// FiveM's memory extensions (gta-streaming-five/src/PatchExtendedBudgeting.cpp): a bigger
/// texture VRAM budget, and more streaming memory on machines with the RAM for it. Both change
/// how the game sizes its memory, so they have to go in while the game starts, before it does.
/// FiveM's pause-menu slider is left out: the scale comes from the config instead.
class MemoryBudget
{
public:
    MemoryBudget() = default;
    MemoryBudget(const MemoryBudget&) = delete;
    MemoryBudget& operator=(const MemoryBudget&) = delete;
    MemoryBudget(MemoryBudget&&) = delete;
    MemoryBudget& operator=(MemoryBudget&&) = delete;
    ~MemoryBudget() = default;

    void Initialize(const GameAddresses& addresses);

    /// Rewrites the texture budget table from a 3 GB base scaled by 1 + scale / 12, and hooks the
    /// graphics menu's estimate and the streamer's free memory, which refuses more once the
    /// loaded list is nearly full. Returns the full budget in bytes. Idempotent.
    [[nodiscard]] Result<uint64_t> ExtendTextureBudget(uint32_t scale);

    /// Doubles the grcResourceCache pool and raises the streaming allocator to 1.5 GiB with 12 GB
    /// of RAM, or to 2 GiB with 16 GB. Refused below 12 GB and when any of the three values is
    /// not the vanilla one. Returns the allocator reservation in bytes. Idempotent.
    [[nodiscard]] Result<uint32_t> ExtendStreamingMemory();

    [[nodiscard]] bool IsTextureBudgetExtended() const
    {
        return m_textureBudgetExtended;
    }

    [[nodiscard]] bool IsStreamingMemoryExtended() const
    {
        return m_streamingMemoryExtended;
    }

private:
    using GetTextureVideoMemoryUsageFn = uint64_t (*)(void* self, int32_t quality, void* settings);
    using GetAvailableMemoryForStreamerFn = uint64_t (*)(void* self);

    static uint64_t GetTextureVideoMemoryUsageDetour(void* self, int32_t quality, void* settings);
    static uint64_t GetAvailableMemoryForStreamerDetour(void* self);

    [[nodiscard]] Result<void> InstallTextureBudgetHooks();

    uintptr_t m_textureBudgetTable = 0;
    uintptr_t m_getTextureVideoMemoryUsage = 0;
    uintptr_t m_getAvailableMemoryForStreamer = 0;
    uintptr_t m_resourceCachePoolSize = 0;
    uintptr_t m_resourceCachePoolLimit = 0;
    uintptr_t m_streamingAllocatorReservation = 0;
    uint64_t m_textureBudgetBytes = 0;
    uint32_t m_streamingAllocatorBytes = 0;
    bool m_textureBudgetExtended = false;
    bool m_streamingMemoryExtended = false;
    memory::PatchRegistry m_patches;

    // The detours run inside game code and have no object to reach, so their state is static.
    static const uint64_t* s_textureBudgetTable;
    static const strStreamingInfoManagerView* s_streamingManager;
    static GetTextureVideoMemoryUsageFn s_getTextureVideoMemoryUsageOriginal;
    static GetAvailableMemoryForStreamerFn s_getAvailableMemoryForStreamerOriginal;
};
} // namespace spl::rage
