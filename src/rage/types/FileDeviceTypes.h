#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace spl::rage
{
/// GetFileAttributes' "no such file" answer, converted at the wrapper and never leaked.
constexpr uint32_t kInvalidFileAttributesRaw = 0xFFFFFFFF;

/// What Open and Create return when there is no file.
constexpr uint64_t kInvalidFileHandleRaw = UINT64_MAX;

/// rage::fiDevice vtable slots. The ordinals come from FiveM's rage-device-five fiDevice.h;
/// unlike the streaming modules, fiDevice's layout does not shift on build 2802.
namespace FileDeviceLayout
{
constexpr size_t kSlotDestructor = 0;
constexpr size_t kSlotOpen = 1;
constexpr size_t kSlotOpenBulk = 2;
constexpr size_t kSlotOpenBulkWrap = 3;
constexpr size_t kSlotCreateLocal = 4;
constexpr size_t kSlotCreate = 5;
constexpr size_t kSlotRead = 6;
constexpr size_t kSlotReadBulk = 7;
constexpr size_t kSlotWriteBulk = 8;
constexpr size_t kSlotWrite = 9;
constexpr size_t kSlotSeek = 10;
constexpr size_t kSlotSeekLong = 11;
constexpr size_t kSlotClose = 12;
constexpr size_t kSlotCloseBulk = 13;
constexpr size_t kSlotGetFileLength = 14;
constexpr size_t kSlotGetFileLengthUInt64 = 15;
constexpr size_t kSlotRemoveFile = 17;
constexpr size_t kSlotRenameFile = 18;
constexpr size_t kSlotCreateDirectory = 19;
constexpr size_t kSlotRemoveDirectory = 20;
constexpr size_t kSlotGetFileLengthLong = 22;
constexpr size_t kSlotGetFileTime = 23;
constexpr size_t kSlotSetFileTime = 24;
constexpr size_t kSlotFindFirst = 25;
constexpr size_t kSlotFindNext = 26;
constexpr size_t kSlotFindClose = 27;
constexpr size_t kSlotResolvePath = 29; ///< FiveM's m_xy(buffer, length, path)
constexpr size_t kSlotTruncate = 30;
constexpr size_t kSlotGetFileAttributes = 31;
constexpr size_t kSlotSetFileAttributes = 33;
constexpr size_t kSlotWriteFull = 36;
constexpr size_t kSlotGetResourceVersion = 37;
constexpr size_t kSlotGetCollectionId = 45;
constexpr size_t kSlotGetName = 46; ///< diagnostics only: verify it before trusting a build

/// Slots FiveM's header declares. A build may have more at the end.
constexpr size_t kKnownSlotCount = 47;
} // namespace FileDeviceLayout

// NOLINTBEGIN(readability-identifier-naming): these keep RAGE's own spelling
/// fiCollection::RawEntry, one loose file of pgRawStreamer. FiveM
/// gta-streaming-five/include/fiCollectionWrapper.h: a 16-byte packfile entry, a timestamp and
/// the path the file was registered under. The retail name getters do not return that path,
/// which is why FiveM replaces GetEntryNameToBuffer with a read of fileName
/// (LoadStreamingFile.cpp:2712, 3787).
struct RawCollectionEntryView
{
    uint64_t packedEntry; // +0x00 nameOffset:16, size:24, offset:24
    uint32_t virtFlags;   // +0x08
    uint32_t physFlags;   // +0x0C
    uint64_t timestamp;   // +0x10
    const char* fileName; // +0x18
};
static_assert(offsetof(RawCollectionEntryView, fileName) == 0x18);
static_assert(sizeof(RawCollectionEntryView) == 0x20);
// NOLINTEND(readability-identifier-naming)

/// rage::fiCollection, which pgRawStreamer is: the fiDevice slots, then its own. FiveM
/// gta-streaming-five/include/fiCollectionWrapper.h. RawStreamerInterface proves the entry
/// list and GetEntryByName against one of the game's own entries before anything relies on them.
namespace CollectionLayout
{
/// const char* GetEntryName(uint16_t index). On retail builds it does not give the registered
/// path (see RawCollectionEntryView), so only diagnostics call it.
constexpr size_t kSlotGetEntryName = FileDeviceLayout::kKnownSlotCount + 4;

/// m_entries: chunkyArray<RawEntry, 1024, 64>, 64 chunk pointers then a uint32 count. FiveM's
/// header puts it after 1448 bytes of fiDevice state, behind the vtable pointer. A fallback only:
/// the offset is read from the game's own GetEntryNameToBuffer when that signature resolves.
constexpr uint32_t kFallbackEntriesOffset = 8 + 1448;
constexpr uint32_t kEntriesPerChunk = 1024;
constexpr uint32_t kEntryChunkCount = 64;
constexpr uint32_t kEntryCountOffset = kEntryChunkCount * static_cast<uint32_t>(sizeof(void*));

/// How far into GetEntryNameToBuffer to look for the chunk load.
constexpr size_t kEntriesLoadSearchBytes = 96;

/// The m_entries offset inside a GetEntryNameToBuffer body: the displacement of its
/// "mov reg, [rcx + chunk*8 + disp32]" (REX.W 8B, ModRM mod=10 rm=SIB, SIB scale=8 base=rcx).
/// std::nullopt when the code has no such load, or loads more than one different offset.
[[nodiscard]] constexpr std::optional<uint32_t> FindEntriesOffset(std::span<const uint8_t> code)
{
    constexpr uint8_t kRexWMask = 0xF8;
    constexpr uint8_t kRexW = 0x48;
    constexpr uint8_t kMovLoad = 0x8B;
    constexpr uint8_t kModRmMask = 0xC7; // mod and rm, any reg
    constexpr uint8_t kModRmDisp32Sib = 0x84;
    constexpr uint8_t kSibMask = 0xC7; // scale and base, any index
    constexpr uint8_t kSibScale8BaseRcx = 0xC1;

    std::optional<uint32_t> found;
    for (size_t at = 0; at + 8 <= code.size(); ++at)
    {
        if ((code[at] & kRexWMask) != kRexW || code[at + 1] != kMovLoad ||
            (code[at + 2] & kModRmMask) != kModRmDisp32Sib ||
            (code[at + 3] & kSibMask) != kSibScale8BaseRcx)
        {
            continue;
        }
        const uint32_t displacement = static_cast<uint32_t>(code[at + 4]) |
                                      (static_cast<uint32_t>(code[at + 5]) << 8) |
                                      (static_cast<uint32_t>(code[at + 6]) << 16) |
                                      (static_cast<uint32_t>(code[at + 7]) << 24);
        if (found && *found != displacement)
        {
            return std::nullopt;
        }
        found = displacement;
    }
    return found;
}

/// uint16_t GetEntryByName(const char* name): the entry for a path, created when it is missing
/// (FiveM Streaming.cpp:214 relies on that for files nobody registered yet).
constexpr size_t kSlotGetEntryByName = FileDeviceLayout::kKnownSlotCount + 6;

/// GetEntryByName's "no entry" answer.
constexpr uint16_t kInvalidEntryIndexRaw = 0xFFFF;

/// Longer than any VFS path the game hands around; a longer "name" is not a name.
constexpr size_t kMaxEntryNameLength = 1024;
} // namespace CollectionLayout
} // namespace spl::rage
