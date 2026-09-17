#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// CItypDependencies and CImapDependencies share this shape: a name hash and the hashes it
/// depends on. FiveM gta-streaming-five/src/LoadStreamingFile.cpp:2257.
struct ManifestDependenciesView
{
    uint32_t name;                      // +0x00
    uint32_t manifestFlags;             // +0x04
    atArrayView<uint32_t> dependencies; // +0x08
};
static_assert(offsetof(ManifestDependenciesView, dependencies) == 0x08);
static_assert(sizeof(ManifestDependenciesView) == 0x18);

/// The game's packfile manifest chunk (CPackFileMetaData), after a .ymf was parsed into it.
/// Only the two dependency arrays are read. FiveM LoadStreamingFile.cpp:2280 — VERIFY on a build.
struct ManifestChunkView
{
    uint8_t pad_0x00[0x10];
    atArrayView<ManifestDependenciesView> mapDataDependencies; // +0x10
    uint8_t pad_0x20[0x10];
    atArrayView<ManifestDependenciesView> mapTypesDependencies; // +0x30
};
static_assert(offsetof(ManifestChunkView, mapDataDependencies) == 0x10);
static_assert(offsetof(ManifestChunkView, mapTypesDependencies) == 0x30);

namespace ManifestChunkLayout
{
/// The name the game's manifest loader reads from. Shared with the game, so it stays mounted
/// only for the duration of one parse (FiveM LoadStreamingFile.cpp:2238).
constexpr std::string_view kMountPoint = "localPack:/";

/// FiveM passes 1 as the packfile argument; the loader only tests it for null.
constexpr uintptr_t kPackfileArgument = 1;

/// FiveM checks that this much of the file reads before it lets the game parse it.
constexpr std::size_t kProbeBytes = 16;

/// A real manifest lists hundreds of maps at most; anything above this is not the array.
constexpr uint16_t kMaxDependencyEntries = 16384;
} // namespace ManifestChunkLayout

/// CInteriorProxy, as FiveM reads it (gta-net-five/src/NetInteriorLocationHacks.cpp:20-40:
/// vtable, mapIndex +8, position +112, archetypeHash +228). Only the map report reads these,
/// and it prints what it found, so a drift shows up as nonsense coordinates rather than a fault.
namespace InteriorProxyLayout
{
constexpr std::size_t kMapDataSlot = 0x08;
constexpr std::size_t kPosition = 0x70;
constexpr std::size_t kArchetypeHash = 0xE4;

/// An entry has to hold the fields above.
constexpr uint32_t kMinEntrySize = kArchetypeHash + sizeof(uint32_t);

/// The vanilla pool holds a few hundred; FiveM raises it to a few thousand.
constexpr uint32_t kMaxPoolSize = 65536;
} // namespace InteriorProxyLayout

namespace MapDataStoreLayout
{
/// fwMapDataStore "should async place", before the 2802 shift. The same slot as the types
/// store's (FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3810).
constexpr std::size_t kSlotShouldAsyncPlace = 29;
} // namespace MapDataStoreLayout

/// The statuses fwMapDataStore and fwStaticBoundsStore pass to ModifyHierarchyStatus. Their
/// meaning is not known; FiveM turns a 1 for one of its own slots into a 2, which keeps the slot
/// active (LoadStreamingFile.cpp:3009).
namespace HierarchyStatusLayout
{
constexpr int32_t kGameStatus = 1;
constexpr int32_t kOverrideStatus = 2;
} // namespace HierarchyStatusLayout

namespace ContentGroupLayout
{
/// The map group of story mode, hashed with JoaatLower. FiveM toggles GROUP_MAP in multiplayer
/// (gta-streaming-five/src/EnableMPMapData.cpp:13).
constexpr std::string_view kStoryMapGroup = "GROUP_MAP_SP";

/// GTA Online's map group: the MP map layers (hei_, apa_, ...) of every multiplayer DLC pack,
/// which FiveM enables at startup instead (gta-streaming-five/src/EnableMPMapData.cpp:16).
constexpr std::string_view kMultiplayerMapGroup = "GROUP_MAP";

/// ClearContentCache(0): the only argument FiveM ever passes.
constexpr int kClearCacheArgument = 0;
} // namespace ContentGroupLayout

/// The temporary rewrite of CFileLoader::LoadChangeSet that makes it rebuild only the map data
/// store (FiveM's ReloadMapStoreNative, gta-streaming-five/src/LoadStreamingFile.cpp:1032).
/// These offsets come from FiveM at one point in time and are the most fragile data in the
/// project: they are applied only on a build whose function bytes are on record, or on request.
namespace ChangeSetReplayLayout
{
/// FiveM backs up this much before patching, so every site lies inside it.
constexpr std::size_t kFunctionBytes = 0x4F3;

/// What LoadChangeSet is called with: two zeroed scratch buffers and a hash nobody uses.
constexpr std::size_t kScratchBufferBytes = 512;
constexpr uint32_t kReplayHash = 0xDEADBDEF;

constexpr uint8_t kCallOpcode = 0xE8;
constexpr uint8_t kJumpOpcode = 0xE9;
constexpr uint8_t kNopOpcode = 0x90;

/// "jmp +0x116" from +0x41 lands in the map-store block at +0x15C.
constexpr int32_t kMapStoreBlockDistance = 0x116;

/// "mov bl, 0": the cache state is treated as empty.
constexpr std::array<uint8_t, 2> kClearCacheState = {0xB3, 0x00};

enum class SiteAction : uint8_t
{
    SkipCall,        ///< a 5-byte call becomes nops; the original must be "E8 rel32"
    JumpToMapStore,  ///< "jmp" straight into the map-store block
    ClearCacheState, ///< "mov bl, 0" followed by nops
    SkipTrailer      ///< nops over the function's tail
};

struct Site
{
    std::ptrdiff_t offset = 0; ///< from the start of LoadChangeSet
    std::size_t sizeBytes = 0;
    SiteAction action = SiteAction::SkipCall;
    std::string_view purpose;
};

constexpr std::array<Site, 8> kSites = {{
    {.offset = 0x28,
     .sizeBytes = 5,
     .action = SiteAction::SkipCall,
     .purpose = "skip the call before r13d is loaded"},
    {.offset = 0x41,
     .sizeBytes = 5,
     .action = SiteAction::JumpToMapStore,
     .purpose = "jump straight into the map-store block"},
    {.offset = 0x300,
     .sizeBytes = 5,
     .action = SiteAction::SkipCall,
     .purpose = "do not load the change set itself"},
    {.offset = 0x356,
     .sizeBytes = 10,
     .action = SiteAction::ClearCacheState,
     .purpose = "do not use the cache state"},
    {.offset = 0x395,
     .sizeBytes = 5,
     .action = SiteAction::SkipCall,
     .purpose = "do not fill the fake array"},
    {.offset = 0x434,
     .sizeBytes = 5,
     .action = SiteAction::SkipCall,
     .purpose = "skip the static bounds store, which crashes"},
    {.offset = 0x489,
     .sizeBytes = 5,
     .action = SiteAction::SkipCall,
     .purpose = "do not clear the fake array"},
    {.offset = 0x4A3,
     .sizeBytes = 54,
     .action = SiteAction::SkipTrailer,
     .purpose = "skip the trailer"},
}};

/// The bytes a site is overwritten with.
[[nodiscard]] inline std::vector<uint8_t> BuildSiteBytes(const Site& site)
{
    std::vector<uint8_t> bytes(site.sizeBytes, kNopOpcode);
    switch (site.action)
    {
        using enum SiteAction;
    case JumpToMapStore:
        bytes[0] = kJumpOpcode;
        for (std::size_t index = 0; index < sizeof(kMapStoreBlockDistance); ++index)
        {
            bytes[1 + index] =
                static_cast<uint8_t>(static_cast<uint32_t>(kMapStoreBlockDistance) >> (8 * index));
        }
        break;
    case ClearCacheState:
        std::ranges::copy(kClearCacheState, bytes.begin());
        break;
    case SkipCall:
    case SkipTrailer:
        break;
    }
    return bytes;
}

/// A build whose LoadChangeSet is known to take these patches: the JoaatExact digest of its
/// first kFunctionBytes bytes, as the loader logs it. Add a row only after the reload worked.
struct VerifiedBuild
{
    uint32_t build = 0;
    uint32_t functionDigest = 0;
};
} // namespace ChangeSetReplayLayout
} // namespace spl::rage
