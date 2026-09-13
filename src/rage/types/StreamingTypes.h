#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace spl::rage
{
/// Index into strStreamingInfoManager::Entries. Strong so it cannot be mixed up with a
/// LocalSlot, which counts from a module's base index (conventions section 4).
struct GlobalIndex
{
    uint32_t value = 0;
    [[nodiscard]] auto operator<=>(const GlobalIndex&) const = default;
};

/// Index inside one streaming module: globalIndex = module.BaseIndex() + localSlot.
struct LocalSlot
{
    uint32_t value = 0;
    [[nodiscard]] auto operator<=>(const LocalSlot&) const = default;
};

/// Entries[i].handle = (collectionIndex << 16) | entryIndex.
struct StreamingHandle
{
    uint32_t value = 0;
    [[nodiscard]] auto operator<=>(const StreamingHandle&) const = default;
};

/// pgRawStreamer, where every loose file the game or the loader registers ends up. FiveM
/// gta-streaming-five/src/LoadStreamingFile.cpp:1924 — handle = (collection << 16) | entry.
constexpr uint32_t kRawCollectionIndex = 0;

[[nodiscard]] constexpr uint32_t CollectionOf(StreamingHandle handle)
{
    return handle.value >> 16;
}

[[nodiscard]] constexpr uint32_t EntryIndexOf(StreamingHandle handle)
{
    return handle.value & 0xFFFF;
}

/// True for a handle that names a loose file rather than an entry inside an .rpf.
[[nodiscard]] constexpr bool IsRawHandle(StreamingHandle handle)
{
    return CollectionOf(handle) == kRawCollectionIndex;
}

/// The handle of one raw streamer entry.
[[nodiscard]] constexpr StreamingHandle MakeRawHandle(uint16_t entryIndex)
{
    return StreamingHandle{(kRawCollectionIndex << 16) | entryIndex};
}

/// Entries[i].flags & 3. FiveM devtools-five/src/StreamingDebug.cpp:38 lists the states as
/// loaded (1), requested (2) and loading (3).
enum class LoadState : uint8_t
{
    NotLoaded = 0,
    Loaded = 1,
    Requested = 2,
    Loading = 3
};

/// What the game writes into an out-parameter when a lookup found nothing. It never leaves
/// the wrapper that makes the call (conventions section 5).
constexpr uint32_t kInvalidSlotRaw = 0xFFFFFFFF;

/// One streaming entry. Copied out of game memory rather than viewed, because it is small
/// and callers keep it. flags & 3 is the load state (see LoadState).
struct StreamingDataEntry
{
    uint32_t handle;
    uint32_t flags;
};
static_assert(sizeof(StreamingDataEntry) == 8);
static_assert(offsetof(StreamingDataEntry, handle) == 0);

/// rage::atArray, the game's growable array. Never constructed by us.
template <typename T> struct atArrayView
{
    T* data;           // +0x00
    uint16_t count;    // +0x08
    uint16_t capacity; // +0x0A
};
static_assert(sizeof(atArrayView<void*>) == 16);
static_assert(offsetof(atArrayView<void*>, count) == 0x08);

/// rage::fwBasePool, the store behind every asset store's entries. GetAt() is index * entrySize
/// into data, valid only while flags[index] >= 0.
struct atPoolView
{
    uint8_t* data;      // +0x00
    int8_t* flags;      // +0x08
    uint32_t size;      // +0x10, the pool's capacity in entries
    uint32_t entrySize; // +0x14
    uint32_t pad_0x18[2];
    uint32_t bitCount; // +0x20
};
static_assert(offsetof(atPoolView, size) == 0x10);
static_assert(offsetof(atPoolView, bitCount) == 0x20);

/// rage::strStreamingModuleMgr, the array of asset stores. It sits inline inside the
/// streaming manager, which is why it has no pointer of its own.
struct strStreamingModuleMgrView
{
    void* vtable; // +0x00
    uint8_t pad_0x08[0x10];
    atArrayView<void*> modules; // +0x18
};
static_assert(offsetof(strStreamingModuleMgrView, modules) == 0x18);
static_assert(sizeof(strStreamingModuleMgrView) == 0x28);

/// rage::strStreamingInfoManager (FiveM's streaming::Manager). Laid over the static instance
/// in GTA5.exe's data section; the padding is everything we do not read.
struct strStreamingInfoManagerView
{
    StreamingDataEntry* entries; // +0x000
    uint8_t pad_0x008[0x10];
    int32_t numEntries; // +0x018
    uint8_t pad_0x01C[0x1B8 - 0x1C];
    strStreamingModuleMgrView moduleMgr; // +0x1B8
    int32_t numPendingRequests;          // +0x1E0
};
static_assert(offsetof(strStreamingInfoManagerView, numEntries) == 0x18);
static_assert(offsetof(strStreamingInfoManagerView, moduleMgr) == 0x1B8);
static_assert(offsetof(strStreamingInfoManagerView, numPendingRequests) == 0x1E0);

[[nodiscard]] constexpr LoadState LoadStateOf(const StreamingDataEntry& entry)
{
    return static_cast<LoadState>(entry.flags & 3);
}

/// Where things live inside one rage::strStreamingModule, and which vtable slot is which.
/// Nothing outside this namespace spells an offset or a slot number.
namespace StreamingModuleLayout
{
constexpr ptrdiff_t kBaseIndex = 0x08; ///< uint32 baseIdx, right after the vtable pointer
constexpr ptrdiff_t kAssetPool = 0x38; ///< the fwAssetStore entry pool

/// uint16 flags inside one pool entry (fwAssetDef). FiveM LoadStreamingFile.cpp:1328.
constexpr ptrdiff_t kAssetDefFlags = 0x10;

/// A .ytyp the game keeps loaded for good (FiveM LoadStreamingFile.cpp:1331).
constexpr uint16_t kAssetFlagPermanent = 0x4;

/// What FiveM clears before releasing a permanent .ytyp it overrides: the permanent bit and 0x10,
/// whose meaning is not known (LoadStreamingFile.cpp:1333).
constexpr uint16_t kAssetFlagsClearedToRelease = 0x14;

// Slot numbers for builds before 2802. FiveM: gta-streaming-five/include/Streaming.h.
constexpr size_t kSlotFindSlotFromHashKey = 1; ///< creates a slot; we never call it here
constexpr size_t kSlotFindSlot = 2;
constexpr size_t kSlotRemove = 3;
constexpr size_t kSlotRemoveSlot = 4;
constexpr size_t kSlotGetPtr = 8;
constexpr size_t kSlotGetNumRefs = 19;
constexpr size_t kSlotGetDependencies = 21;

/// Build 2802 inserted six virtuals at the top of the class, which shifts every slot after
/// it. FiveM expresses the same thing as XBR_VIRTUAL_BASE_2802(0) with offset 6.
[[nodiscard]] constexpr size_t VtableShift(uint32_t build)
{
    return build >= 2802 ? 6 : 0;
}
} // namespace StreamingModuleLayout
} // namespace spl::rage
