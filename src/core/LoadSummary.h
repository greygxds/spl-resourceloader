#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "resource/Resource.h"
#include "streaming/AssetRegistry.h"
#include "streaming/StreamingPlan.h"

namespace spl
{
/// A folder of a mod mounted over one of the game's mount points.
struct OverlayMount
{
    std::filesystem::path folder; ///< "<mods folder>/<mod>/common", not on disk
    std::string mountPoint;       ///< "common:/"
};

/// The info lines a finished launch ends with: one per resource and per mod, each group closed by
/// its totals. A group with no entries has no lines at all.
struct LoadSummary
{
    std::vector<std::string> resources;
    std::string resourceTotals;
    std::vector<std::string> mods;
    std::string modTotals;
};

/// Everything the summary counts, all owned by the caller.
struct LoadSummaryInput
{
    std::span<const resource::Resource> resources; ///< in load order; ResourceId is the index
    const streaming::StreamingPlan* plan = nullptr;
    const streaming::AssetRegistry* registry = nullptr;
    std::span<const OverlayMount> overlays;
};

/// Pure: it reads the plan and the registry and formats, nothing else.
[[nodiscard]] LoadSummary BuildLoadSummary(const LoadSummaryInput& input);

/// "1 asset" or "6 assets".
[[nodiscard]] std::string CountOf(std::size_t count, std::string_view singular,
                                  std::string_view plural);
} // namespace spl
