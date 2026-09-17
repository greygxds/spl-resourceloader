#include "rage/MapStoreReloader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "memory/CodePatch.h"
#include "rage/SafeCall.h"
#include "rage/types/MapStoreTypes.h"
#include "util/Hash.h"

namespace spl::rage
{
namespace
{
using namespace ChangeSetReplayLayout;

using LoadChangeSetFn = void (*)(void* changeSet, void* scratch, uint32_t* hash);
using VoidFn = void (*)();
using ContentGroupFn = void (*)(void* manager, uint32_t groupHash);
using ClearContentCacheFn = void (*)(int);

/// Builds whose LoadChangeSet took the replay patches in game. Informational: the replay runs on
/// any build whose patch sites are intact, and this only changes what the log says. The digest to
/// add is the one the loader logs.
constexpr std::array<VerifiedBuild, 1> kVerifiedBuilds{{
    // 2026-09-13: smeggo_clockmaker's ymap, MLO and ybn all stream in; the toggle left them out.
    {.build = 3889, .functionDigest = 0xc6695703},
}};

/// FiveM's batch size (LoadStreamingFile.cpp:1119): each batch blocks until it has loaded.
constexpr std::size_t kCollisionBatchSize = 4;

[[nodiscard]] int64_t MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

[[nodiscard]] std::string ToHex(std::span<const uint8_t> bytes)
{
    std::string text;
    for (const uint8_t byte : bytes)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += fmt::format("{:02X}", byte);
    }
    return text;
}

[[nodiscard]] std::optional<std::vector<uint8_t>> ReadBytes(uintptr_t address, std::size_t count)
{
    std::vector<uint8_t> bytes(count);
    const bool read =
        SafeCall("MapStoreReloader::ReadCode",
                 [&] { std::memcpy(bytes.data(), reinterpret_cast<void*>(address), count); });
    if (!read)
    {
        return std::nullopt;
    }
    return bytes;
}
} // namespace

std::string_view ToString(MapReloadMethod method)
{
    using enum MapReloadMethod;
    switch (method)
    {
    case ChangeSetReplay:
        return "change set replay";
    case ContentGroupToggle:
        return "content group toggle";
    }
    return "content group toggle";
}

void MapStoreReloader::Initialize(const GameAddresses& addresses, const GameBuild& build)
{
    m_loadChangeSet = addresses.fileLoaderLoadChangeSet;
    m_reloadMapIfNeeded = addresses.reloadMapIfNeeded;
    m_contentManagerInstance = addresses.extraContentManagerInstance;
    m_disableContentGroup = addresses.extraContentManagerDisableContentGroup;
    m_enableContentGroup = addresses.extraContentManagerEnableContentGroup;
    m_clearContentCache = addresses.extraContentManagerClearContentCache;
    m_build = build.build;
}

bool MapStoreReloader::CanReplayChangeSet() const
{
    return m_loadChangeSet != 0 && m_reloadMapIfNeeded != 0;
}

bool MapStoreReloader::CanToggleContentGroup() const
{
    return m_contentManagerInstance != 0 && m_disableContentGroup != 0 && m_enableContentGroup != 0;
}

Result<void> MapStoreReloader::Verify(const memory::Module& image,
                                      const StreamingInterface& streaming) const
{
    if (!streaming.CanLoadObjects())
    {
        return MakeError(ErrorCode::NotFound,
                         "the streaming request, load and release signatures did not all resolve, "
                         "so collisions cannot be preloaded");
    }
    if (!image.Contains(m_clearContentCache))
    {
        return MakeError(ErrorCode::NotFound,
                         "signature 'CExtraContentManager::ClearContentCache' did not resolve");
    }
    if (!CanReplayChangeSet() && !CanToggleContentGroup())
    {
        return MakeError(ErrorCode::NotFound,
                         "neither the change-set replay nor the content-group toggle signatures "
                         "resolved");
    }
    return {};
}

MapStoreReloader::ChangeSetInspection MapStoreReloader::InspectChangeSet() const
{
    ChangeSetInspection inspection;
    if (!CanReplayChangeSet())
    {
        return inspection;
    }
    const std::optional<std::vector<uint8_t>> function = ReadBytes(m_loadChangeSet, kFunctionBytes);
    if (!function)
    {
        return inspection;
    }

    inspection.readable = true;
    inspection.digest = util::JoaatExact(
        std::string_view(reinterpret_cast<const char*>(function->data()), function->size()));
    inspection.callSitesIntact = std::ranges::all_of(
        kSites, [&](const Site& site)
        { return site.action != SiteAction::SkipCall || (*function)[site.offset] == kCallOpcode; });
    inspection.onRecord = std::ranges::any_of(
        kVerifiedBuilds, [&](const VerifiedBuild& verified)
        { return verified.build == m_build && verified.functionDigest == inspection.digest; });
    return inspection;
}

void MapStoreReloader::LogChangeSetSites() const
{
    for (const Site& site : kSites)
    {
        const std::optional<std::vector<uint8_t>> bytes =
            ReadBytes(m_loadChangeSet + site.offset, site.sizeBytes);
        SPL_LOG_DEBUG(Rage, "LoadChangeSet+{:#x} ({}): {}", site.offset, site.purpose,
                      bytes ? ToHex(*bytes) : std::string{"unreadable"});
    }
}

Result<MapReloadMethod> MapStoreReloader::ChooseMethod(config::MapReloadStrategy strategy) const
{
    const auto fallBack = [&](std::string_view reason) -> Result<MapReloadMethod>
    {
        if (!CanToggleContentGroup())
        {
            return MakeError(ErrorCode::NotFound,
                             "{}, and the content-group toggle signatures did not resolve", reason);
        }
        // On 3889 the toggle rebuilt the store without giving new .ymap files a place in the box
        // streamer, so they never streamed in. Asked for, it is the
        // user's choice; as a fallback it is worth a warning.
        if (strategy == config::MapReloadStrategy::ContentGroupToggle)
        {
            SPL_LOG_INFO(Rage, "Rebuilding the map store by {}: {}",
                         ToString(MapReloadMethod::ContentGroupToggle), reason);
        }
        else
        {
            SPL_LOG_WARNING(Rage,
                            "Rebuilding the map store by {}: {}. New .ymap files may not stream "
                            "in this way",
                            ToString(MapReloadMethod::ContentGroupToggle), reason);
        }
        return MapReloadMethod::ContentGroupToggle;
    };

    if (strategy == config::MapReloadStrategy::ContentGroupToggle)
    {
        return fallBack("diagnostics.map_reload_strategy asks for it");
    }
    if (!CanReplayChangeSet())
    {
        return fallBack("the change-set replay signatures did not resolve");
    }

    // The replay is the default on every build, because the toggle does not stream new maps. What
    // keeps it safe on a build nobody has run is the site check, which refuses code that moved.
    const ChangeSetInspection inspection = InspectChangeSet();
    if (!inspection.readable || !inspection.callSitesIntact)
    {
        LogChangeSetSites();
        return fallBack("LoadChangeSet does not have the calls the replay patches expect");
    }

    if (inspection.onRecord)
    {
        SPL_LOG_DEBUG(Rage,
                      "Rebuilding the map store by {} (LoadChangeSet {:#010x} is verified for "
                      "build {})",
                      ToString(MapReloadMethod::ChangeSetReplay), inspection.digest, m_build);
    }
    else
    {
        LogChangeSetSites();
        SPL_LOG_DEBUG(Rage,
                      "Rebuilding the map store by {} (LoadChangeSet {:#010x} is not on record for "
                      "build {}, but every patch site is intact)",
                      ToString(MapReloadMethod::ChangeSetReplay), inspection.digest, m_build);
    }
    return MapReloadMethod::ChangeSetReplay;
}

Result<std::size_t>
MapStoreReloader::PreloadCollisions(const StreamingInterface& streaming,
                                    std::span<const GlobalIndex> collisions) const
{
    for (std::size_t batchStart = 0; batchStart < collisions.size();
         batchStart += kCollisionBatchSize)
    {
        const std::span<const GlobalIndex> batch = collisions.subspan(
            batchStart, std::min(kCollisionBatchSize, collisions.size() - batchStart));
        for (const GlobalIndex index : batch)
        {
            if (!streaming.RequestObject(index, 0))
            {
                return MakeError(ErrorCode::Unavailable, "requesting collision #{} failed",
                                 index.value);
            }
        }
        if (!streaming.LoadAllRequestedObjects())
        {
            return MakeError(ErrorCode::Unavailable, "loading the requested collisions failed");
        }
        for (const GlobalIndex index : batch)
        {
            streaming.ReleaseObject(index);
        }
        SPL_LOG_DEBUG(Rage, "Preloaded {}/{} collision(s)", batchStart + batch.size(),
                      collisions.size());
    }
    return collisions.size();
}

Result<void> MapStoreReloader::ReplayChangeSet() const
{
    // Everything written here is undone before this function returns, faults included: the
    // registry restores on destruction, and a caught fault returns normally.
    memory::PatchRegistry patches;
    for (const Site& site : kSites)
    {
        const std::vector<uint8_t> bytes = BuildSiteBytes(site);
        if (Result<void> applied = patches.Apply(fmt::format("ChangeSetReplay+{:#x}", site.offset),
                                                 m_loadChangeSet + site.offset, bytes);
            !applied)
        {
            return applied;
        }
    }

    std::array<uint8_t, kScratchBufferBytes> changeSet{};
    std::array<uint8_t, kScratchBufferBytes> scratch{};
    uint32_t hash = kReplayHash;
    const auto loadChangeSet = reinterpret_cast<LoadChangeSetFn>(m_loadChangeSet);
    const bool replayed = SafeCall("CFileLoader::LoadChangeSet",
                                   [&] { loadChangeSet(changeSet.data(), scratch.data(), &hash); });
    patches.RestoreAll();
    if (!replayed)
    {
        return MakeError(ErrorCode::Unavailable, "the patched LoadChangeSet faulted");
    }

    const auto reloadMapIfNeeded = reinterpret_cast<VoidFn>(m_reloadMapIfNeeded);
    if (!SafeCall("ReloadMapIfNeeded", [&] { reloadMapIfNeeded(); }))
    {
        return MakeError(ErrorCode::Unavailable, "ReloadMapIfNeeded faulted");
    }
    return {};
}

Result<void> MapStoreReloader::ToggleContentGroup() const
{
    const std::optional<void*> manager =
        SafeCall("CExtraContentManager::sm_instance",
                 [&] { return *reinterpret_cast<void* const*>(m_contentManagerInstance); });
    if (!manager || *manager == nullptr)
    {
        return MakeError(ErrorCode::Unavailable, "the extra content manager does not exist yet");
    }

    const uint32_t group = util::JoaatLower(m_mapGroup);
    const auto disable = reinterpret_cast<ContentGroupFn>(m_disableContentGroup);
    const auto enable = reinterpret_cast<ContentGroupFn>(m_enableContentGroup);
    if (!SafeCall("CExtraContentManager::DisableContentGroup", [&] { disable(*manager, group); }))
    {
        return MakeError(ErrorCode::Unavailable, "disabling {} faulted", m_mapGroup);
    }
    if (!SafeCall("CExtraContentManager::EnableContentGroup", [&] { enable(*manager, group); }))
    {
        return MakeError(ErrorCode::Unavailable, "enabling {} faulted", m_mapGroup);
    }
    return {};
}

Result<MapReloadTimings> MapStoreReloader::Reload(const StreamingInterface& streaming,
                                                  std::span<const GlobalIndex> collisions,
                                                  config::MapReloadStrategy strategy)
{
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }

    const auto started = std::chrono::steady_clock::now();
    MapReloadTimings timings;

    Result<std::size_t> preloaded = PreloadCollisions(streaming, collisions);
    if (!preloaded)
    {
        return preloaded.GetError();
    }
    timings.collisionsPreloaded = preloaded.GetValue();
    timings.preloadMs = MillisecondsSince(started);

    Result<MapReloadMethod> method = ChooseMethod(strategy);
    if (!method)
    {
        return method.GetError();
    }
    timings.method = method.GetValue();

    Result<void> rebuilt = timings.method == MapReloadMethod::ChangeSetReplay
                               ? ReplayChangeSet()
                               : ToggleContentGroup();
    // A patch that would not apply never ran any game code, so the toggle is still safe.
    if (!rebuilt && timings.method == MapReloadMethod::ChangeSetReplay && !HasFaulted() &&
        CanToggleContentGroup())
    {
        SPL_LOG_WARNING(Rage, "Change set replay failed ({}); falling back to {}",
                        rebuilt.GetMessage(), ToString(MapReloadMethod::ContentGroupToggle));
        timings.method = MapReloadMethod::ContentGroupToggle;
        rebuilt = ToggleContentGroup();
    }
    if (!rebuilt)
    {
        return rebuilt.GetError();
    }

    const auto clearContentCache = reinterpret_cast<ClearContentCacheFn>(m_clearContentCache);
    if (!SafeCall("CExtraContentManager::ClearContentCache",
                  [&] { clearContentCache(ContentGroupLayout::kClearCacheArgument); }))
    {
        return MakeError(ErrorCode::Unavailable, "ClearContentCache faulted");
    }

    timings.totalMs = MillisecondsSince(started);
    return timings;
}
} // namespace spl::rage
