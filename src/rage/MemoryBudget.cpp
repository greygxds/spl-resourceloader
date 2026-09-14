#include "rage/MemoryBudget.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

#include "hooking/HookManager.h"
#include "logging/Logger.h"
#include "memory/Module.h"
#include "platform/Win32.h"
#include "rage/SafeCall.h"
#include "rage/types/MemoryBudgetTypes.h"

namespace spl::rage
{
namespace
{
constexpr std::string_view kTextureVideoMemoryUsageHook = "GetTextureVideoMemoryUsage";
constexpr std::string_view kAvailableMemoryForStreamerHook = "GetAvailableMemoryForStreamer";
constexpr std::string_view kResourceCachePoolSizePatch = "ExtendedResourceCachePool";
constexpr std::string_view kResourceCachePoolLimitPatch = "ExtendedResourceCacheLimit";
constexpr std::string_view kStreamingAllocatorPatch = "ExtendedStreamingAllocator";

using TextureBudgetTable = std::array<uint64_t, MemoryBudgetLayout::kTableEntries>;

[[nodiscard]] std::string DescribeAddress(uintptr_t address)
{
    const memory::Module image = memory::Module::Main();
    return image.Contains(address)
               ? fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase())
               : fmt::format("{:#x}", address);
}

[[nodiscard]] std::array<uint8_t, 4> ToBytes(uint32_t value)
{
    std::array<uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &value, bytes.size());
    return bytes;
}

[[nodiscard]] double ToGibibytes(uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

[[nodiscard]] std::optional<uint64_t> ReadTotalPhysicalBytes()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status) == FALSE)
    {
        return std::nullopt;
    }
    return status.ullTotalPhys;
}

/// Every entry of a real budget table is a size a GPU could have; a wrong pointer is unlikely to
/// look like eighty of them.
[[nodiscard]] bool IsPlausibleBudgetTable(const TextureBudgetTable& table)
{
    const bool inRange =
        std::ranges::all_of(table, [](uint64_t bytes)
                            { return bytes <= MemoryBudgetLayout::kPlausibleBudgetLimitBytes; });
    const bool populated = std::ranges::any_of(table, [](uint64_t bytes) { return bytes != 0; });
    return inRange && populated;
}
} // namespace

const uint64_t* MemoryBudget::s_textureBudgetTable = nullptr;
const strStreamingInfoManagerView* MemoryBudget::s_streamingManager = nullptr;
MemoryBudget::GetTextureVideoMemoryUsageFn MemoryBudget::s_getTextureVideoMemoryUsageOriginal =
    nullptr;
MemoryBudget::GetAvailableMemoryForStreamerFn
    MemoryBudget::s_getAvailableMemoryForStreamerOriginal = nullptr;

void MemoryBudget::Initialize(const GameAddresses& addresses)
{
    m_textureBudgetTable = addresses.textureBudgetTable;
    m_getTextureVideoMemoryUsage = addresses.getTextureVideoMemoryUsage;
    m_getAvailableMemoryForStreamer = addresses.getAvailableMemoryForStreamer;
    m_resourceCachePoolSize = addresses.resourceCachePoolSize;
    m_resourceCachePoolLimit = addresses.resourceCachePoolLimit;
    m_streamingAllocatorReservation = addresses.streamingAllocatorReservation;
    s_streamingManager = reinterpret_cast<const strStreamingInfoManagerView*>(
        addresses.streamingInfoManagerInstance);
}

Result<uint64_t> MemoryBudget::ExtendTextureBudget(uint32_t scale)
{
    if (m_textureBudgetExtended)
    {
        return m_textureBudgetBytes;
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted; no patch is applied");
    }

    const memory::Module image = memory::Module::Main();
    if (!image.Contains(m_textureBudgetTable) || m_getTextureVideoMemoryUsage == 0 ||
        m_getAvailableMemoryForStreamer == 0 || s_streamingManager == nullptr)
    {
        return MakeError(ErrorCode::NotFound,
                         "the texture budget signatures did not resolve on this build");
    }

    const std::optional<TextureBudgetTable> current =
        SafeCall("MemoryBudget::ReadTextureBudgetTable",
                 [&]
                 {
                     TextureBudgetTable table{};
                     std::memcpy(table.data(), reinterpret_cast<const void*>(m_textureBudgetTable),
                                 sizeof(table));
                     return table;
                 });
    if (!current || !IsPlausibleBudgetTable(*current))
    {
        return MakeError(ErrorCode::NotFound,
                         "{} does not hold a texture budget table, so the signature no longer "
                         "points at it",
                         DescribeAddress(m_textureBudgetTable));
    }

    // The streamer's guard goes in before the bigger budget can fill its loaded list.
    if (Result<void> hooked = InstallTextureBudgetHooks(); !hooked)
    {
        return hooked.GetError();
    }

    const std::array<uint64_t, MemoryBudgetLayout::kQualityLevels> row = TextureBudgetRow(scale);
    TextureBudgetTable table{};
    for (std::size_t entry = 0; entry < table.size(); ++entry)
    {
        table[entry] = row[entry % MemoryBudgetLayout::kQualityLevels];
    }
    const bool written = SafeCall("MemoryBudget::WriteTextureBudgetTable",
                                  [&]
                                  {
                                      std::memcpy(reinterpret_cast<void*>(m_textureBudgetTable),
                                                  table.data(), sizeof(table));
                                  });
    if (!written)
    {
        return MakeError(ErrorCode::Unavailable, "the texture budget table at {} is not writable",
                         DescribeAddress(m_textureBudgetTable));
    }

    s_textureBudgetTable = reinterpret_cast<const uint64_t*>(m_textureBudgetTable);
    m_textureBudgetBytes = row.back();
    m_textureBudgetExtended = true;
    SPL_LOG_DEBUG(Hook, "Texture budget table at {} rewritten: {:.2f} GiB (scale {})",
                  DescribeAddress(m_textureBudgetTable), ToGibibytes(m_textureBudgetBytes), scale);
    return m_textureBudgetBytes;
}

Result<void> MemoryBudget::InstallTextureBudgetHooks()
{
    hooking::HookManager& hooks = hooking::HookManager::Instance();
    if (Result<void> ready = hooks.Initialize(); !ready)
    {
        return ready;
    }

    if (!hooks.FindStatus(kAvailableMemoryForStreamerHook))
    {
        if (Result<void> created = hooks.Create(
                kAvailableMemoryForStreamerHook, m_getAvailableMemoryForStreamer,
                &GetAvailableMemoryForStreamerDetour, &s_getAvailableMemoryForStreamerOriginal);
            !created)
        {
            return created;
        }
    }
    if (Result<void> enabled = hooks.Enable(kAvailableMemoryForStreamerHook); !enabled)
    {
        return enabled;
    }

    if (!hooks.FindStatus(kTextureVideoMemoryUsageHook))
    {
        if (Result<void> created = hooks.Create(
                kTextureVideoMemoryUsageHook, m_getTextureVideoMemoryUsage,
                &GetTextureVideoMemoryUsageDetour, &s_getTextureVideoMemoryUsageOriginal);
            !created)
        {
            return created;
        }
    }
    if (Result<void> enabled = hooks.Enable(kTextureVideoMemoryUsageHook); !enabled)
    {
        return enabled;
    }

    SPL_LOG_DEBUG(Hook, "Hooks '{}' at {} and '{}' at {} installed",
                  kAvailableMemoryForStreamerHook, DescribeAddress(m_getAvailableMemoryForStreamer),
                  kTextureVideoMemoryUsageHook, DescribeAddress(m_getTextureVideoMemoryUsage));
    return {};
}

Result<uint32_t> MemoryBudget::ExtendStreamingMemory()
{
    if (m_streamingMemoryExtended)
    {
        return m_streamingAllocatorBytes;
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted; no patch is applied");
    }

    const std::optional<uint64_t> totalPhysicalBytes = ReadTotalPhysicalBytes();
    if (!totalPhysicalBytes)
    {
        return MakeError(ErrorCode::Unavailable, "the amount of system RAM could not be read");
    }
    const std::optional<uint32_t> allocatorBytes = StreamingAllocatorBytesFor(*totalPhysicalBytes);
    if (!allocatorBytes)
    {
        return MakeError(ErrorCode::NotSupported,
                         "this system has {:.1f} GiB of RAM, and it takes 12 GB or more",
                         ToGibibytes(*totalPhysicalBytes));
    }

    const memory::Module image = memory::Module::Main();
    if (!image.Contains(m_resourceCachePoolSize) || !image.Contains(m_resourceCachePoolLimit) ||
        !image.Contains(m_streamingAllocatorReservation))
    {
        return MakeError(ErrorCode::NotFound,
                         "the streaming memory signatures did not resolve on this build");
    }

    // All three are checked before any is written, so a drifted signature changes nothing.
    struct Imm32Patch
    {
        std::string_view name;
        uintptr_t address;
        uint32_t vanilla;
        uint32_t extended;
    };
    const std::array<Imm32Patch, 3> patches = {{
        {kResourceCachePoolSizePatch, m_resourceCachePoolSize,
         MemoryBudgetLayout::kVanillaResourceCacheEntries,
         MemoryBudgetLayout::kExtendedResourceCacheEntries},
        {kResourceCachePoolLimitPatch, m_resourceCachePoolLimit,
         MemoryBudgetLayout::kVanillaResourceCacheLimit,
         MemoryBudgetLayout::kExtendedResourceCacheLimit},
        {kStreamingAllocatorPatch, m_streamingAllocatorReservation,
         MemoryBudgetLayout::kVanillaStreamingAllocatorBytes, *allocatorBytes},
    }};
    for (const Imm32Patch& patch : patches)
    {
        const std::optional<uint32_t> current =
            SafeCall("MemoryBudget::ReadImm32",
                     [&] { return *reinterpret_cast<const uint32_t*>(patch.address); });
        if (!current || *current != patch.vanilla)
        {
            return MakeError(ErrorCode::NotFound,
                             "patch '{}' refused: {} does not hold {:#x}, so the signature no "
                             "longer points at it or another mod changed it",
                             patch.name, DescribeAddress(patch.address), patch.vanilla);
        }
    }
    for (const Imm32Patch& patch : patches)
    {
        if (Result<void> applied = m_patches.Apply(std::string{patch.name}, patch.address,
                                                   ToBytes(patch.extended), ToBytes(patch.vanilla));
            !applied)
        {
            return applied.GetError();
        }
        SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {} ({:#x})", patch.name,
                      DescribeAddress(patch.address), patch.extended);
    }

    m_streamingAllocatorBytes = *allocatorBytes;
    m_streamingMemoryExtended = true;
    return m_streamingAllocatorBytes;
}

uint64_t MemoryBudget::GetTextureVideoMemoryUsageDetour(void* self, int32_t quality, void* settings)
{
    const uint64_t original = s_getTextureVideoMemoryUsageOriginal(self, quality, settings);
    const int32_t column = quality + 1; // FiveM reads the first row one quality up (:99)
    if (s_textureBudgetTable == nullptr || column < 0 ||
        column >= static_cast<int32_t>(MemoryBudgetLayout::kQualityLevels))
    {
        return original;
    }
    const uint64_t budget = s_textureBudgetTable[column];
    return budget > MemoryBudgetLayout::kMenuTextureUsageOffsetBytes
               ? budget - MemoryBudgetLayout::kMenuTextureUsageOffsetBytes
               : original;
}

uint64_t MemoryBudget::GetAvailableMemoryForStreamerDetour(void* self)
{
    if (s_streamingManager != nullptr &&
        s_streamingManager->loadedListCount >= MemoryBudgetLayout::kMaxLoadedListCount)
    {
        return 0;
    }
    return s_getAvailableMemoryForStreamerOriginal(self);
}
} // namespace spl::rage
