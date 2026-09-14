#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace spl::rage
{
/// The numbers behind FiveM's memory extensions. Every value comes from FiveM
/// gta-streaming-five/src/PatchExtendedBudgeting.cpp, which is cited per line.
namespace MemoryBudgetLayout
{
/// FiveM counts a gigabyte as 1000 MiB so that hardware-reserved memory still counts (:19).
constexpr uint64_t kGigabyteBytes = 1000ull * 1024 * 1024;

/// The texture budget table: rows of four quality levels, 80 uint64 in all (:70).
constexpr std::size_t kQualityLevels = 4;
constexpr std::size_t kTableRows = 20;
constexpr std::size_t kTableEntries = kQualityLevels * kTableRows;

constexpr uint64_t kBaseTextureBudgetBytes = 3 * kGigabyteBytes; ///< :168
constexpr uint32_t kMaxTextureBudgetScale = 12;                  ///< :35

/// A value no real budget reaches, to tell the table from a wrong pointer before writing to it.
constexpr uint64_t kPlausibleBudgetLimitBytes = 64 * kGigabyteBytes;

/// The graphics menu reports texture memory as the budget minus this (:98).
constexpr uint64_t kMenuTextureUsageOffsetBytes = kGigabyteBytes;

/// The streamer is refused memory once its loaded list nears its ~32k capacity (:108).
constexpr uint32_t kMaxLoadedListCount = 30000;

/// grcResourceCache pool size and the limit next to it (:176-177). The vanilla limit is FiveM's
/// value less the doubled pool size, which the expected-byte check confirms before writing.
constexpr uint32_t kVanillaResourceCacheEntries = 0x50000;
constexpr uint32_t kExtendedResourceCacheEntries = 0xA0000;
constexpr uint32_t kVanillaResourceCacheLimit = 0x5001B;
constexpr uint32_t kExtendedResourceCacheLimit = 0xA001B;

/// The streaming allocator's reservation, and what FiveM raises it to by system RAM (:148-161).
constexpr uint32_t kVanillaStreamingAllocatorBytes = 0x40000000;
constexpr uint32_t kLargeStreamingAllocatorBytes = 0x60000000;
constexpr uint32_t kHugeStreamingAllocatorBytes = 0x7FFFFFFF;
constexpr uint64_t kLargeSystemMemoryBytes = 12 * kGigabyteBytes;
constexpr uint64_t kHugeSystemMemoryBytes = 16 * kGigabyteBytes;
} // namespace MemoryBudgetLayout

/// One row of the texture budget table, lowest quality first. The game's texture flag counts
/// mips to cut, so "high" and "very high" both get the full budget, "normal" two thirds and the
/// unused "low" half (PatchExtendedBudgeting.cpp:53-76).
[[nodiscard]] constexpr std::array<uint64_t, MemoryBudgetLayout::kQualityLevels>
TextureBudgetRow(uint32_t scale)
{
    using namespace MemoryBudgetLayout;
    const double multiplier =
        static_cast<double>(std::min(scale, kMaxTextureBudgetScale)) / kMaxTextureBudgetScale + 1.0;
    const auto full =
        static_cast<uint64_t>(static_cast<double>(kBaseTextureBudgetBytes) * multiplier);
    return {static_cast<uint64_t>(static_cast<double>(full) / 2.0),
            static_cast<uint64_t>(static_cast<double>(full) / 1.5), full, full};
}

/// The streaming allocator reservation for a machine with this much RAM, or std::nullopt below
/// 12 GB, where FiveM leaves streaming memory alone because it breaks small systems (:165).
[[nodiscard]] constexpr std::optional<uint32_t>
StreamingAllocatorBytesFor(uint64_t totalPhysicalBytes)
{
    using namespace MemoryBudgetLayout;
    if (totalPhysicalBytes >= kHugeSystemMemoryBytes)
    {
        return kHugeStreamingAllocatorBytes;
    }
    if (totalPhysicalBytes >= kLargeSystemMemoryBytes)
    {
        return kLargeStreamingAllocatorBytes;
    }
    return std::nullopt;
}
} // namespace spl::rage
