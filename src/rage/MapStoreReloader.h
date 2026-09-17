#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"
#include "rage/GameBuild.h"
#include "rage/StreamingInterface.h"
#include "rage/types/MapStoreTypes.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// How the map data store is rebuilt. Auto is only a request; a reload always uses one of these.
enum class MapReloadMethod
{
    ChangeSetReplay,   ///< FiveM's ReloadMapStoreNative: patched LoadChangeSet, map store only
    ContentGroupToggle ///< disable and re-enable the active map group: heavier, but no patching
};

[[nodiscard]] std::string_view ToString(MapReloadMethod method);

struct MapReloadTimings
{
    MapReloadMethod method = MapReloadMethod::ContentGroupToggle;
    std::size_t collisionsPreloaded = 0;
    int64_t preloadMs = 0;
    int64_t totalMs = 0;
};

/// Makes .ymap and .ybn files registered after the session started visible to the world:
/// their bounds are loaded once, the map data store is rebuilt so the box
/// streamers learn their extents, and the content cache is cleared.
class MapStoreReloader
{
public:
    void Initialize(const GameAddresses& addresses, const GameBuild& build);

    /// The map group the game runs with, which a ContentGroupToggle reload toggles. Story mode's
    /// unless streaming.mp_maps switched the game to GTA Online's.
    void SetMapGroup(std::string_view group)
    {
        m_mapGroup = group;
    }

    /// Proves that at least one rebuild method and the collision preload are usable. An error
    /// disables map reloads, nothing else.
    [[nodiscard]] Result<void> Verify(const memory::Module& image,
                                      const StreamingInterface& streaming) const;

    /// Runs on the main thread and blocks for as long as the preload and the rebuild take.
    [[nodiscard]] Result<MapReloadTimings> Reload(const StreamingInterface& streaming,
                                                  std::span<const GlobalIndex> collisions,
                                                  config::MapReloadStrategy strategy);

private:
    /// What the bytes of LoadChangeSet say about patching it on this build.
    struct ChangeSetInspection
    {
        bool readable = false;
        bool callSitesIntact = false; ///< every call the replay nops is still "E8 rel32"
        bool onRecord = false;        ///< the digest is in kVerifiedBuilds (log only)
        uint32_t digest = 0;
    };

    [[nodiscard]] bool CanReplayChangeSet() const;
    [[nodiscard]] bool CanToggleContentGroup() const;

    [[nodiscard]] ChangeSetInspection InspectChangeSet() const;

    /// Picks the method for this reload, and says why at info.
    [[nodiscard]] Result<MapReloadMethod> ChooseMethod(config::MapReloadStrategy strategy) const;

    [[nodiscard]] Result<std::size_t>
    PreloadCollisions(const StreamingInterface& streaming,
                      std::span<const GlobalIndex> collisions) const;
    [[nodiscard]] Result<void> ReplayChangeSet() const;
    [[nodiscard]] Result<void> ToggleContentGroup() const;

    /// One debug line per site with its current bytes, to record when a build is verified.
    void LogChangeSetSites() const;

    uintptr_t m_loadChangeSet = 0;
    uintptr_t m_reloadMapIfNeeded = 0;
    uintptr_t m_contentManagerInstance = 0;
    uintptr_t m_disableContentGroup = 0;
    uintptr_t m_enableContentGroup = 0;
    uintptr_t m_clearContentCache = 0;
    uint32_t m_build = 0;
    std::string_view m_mapGroup = ContentGroupLayout::kStoryMapGroup; ///< always a literal
};
} // namespace spl::rage
