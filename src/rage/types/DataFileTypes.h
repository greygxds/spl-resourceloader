#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace spl::rage
{
/// Index into CDataFileMount::sm_Interfaces, as the game's type table maps a type name to it.
struct DataFileTypeIndex
{
    int32_t value = 0;
    [[nodiscard]] auto operator<=>(const DataFileTypeIndex&) const = default;
};

// NOLINTBEGIN(readability-identifier-naming): these keep RAGE's own spelling
/// CDataFileMgr::DataFile, the entry a data-file mounter is handed. We allocate these ourselves,
/// zeroed, so only the fields we write need to be right. FiveM
/// gta-streaming-five/src/LoadStreamingFile.cpp:734 (its offset comments are off by 4).
struct DataFileEntryView
{
    char name[128]; // +0x00
    uint8_t pad_0x80[16];
    int32_t type;    // +0x90
    int32_t index;   // +0x94
    bool locked;     // +0x98
    bool flag2;      // +0x99
    bool flag3;      // +0x9A
    bool disabled;   // +0x9B
    bool persistent; // +0x9C
    bool overlay;    // +0x9D
    uint8_t pad_0x9E[10];
};
static_assert(offsetof(DataFileEntryView, type) == 0x90);
static_assert(offsetof(DataFileEntryView, disabled) == 0x9B);
static_assert(offsetof(DataFileEntryView, persistent) == 0x9C);
static_assert(sizeof(DataFileEntryView) == 0xA8);

/// One row of the game's data-file type table: the type name's JoaatExact hash, and its index.
struct DataFileTypeEnumEntryView
{
    uint32_t hash;  // +0x00
    uint32_t index; // +0x04
};
static_assert(sizeof(DataFileTypeEnumEntryView) == 8);
// NOLINTEND(readability-identifier-naming)

namespace DataFileLayout
{
/// The longest name the entry holds, leaving room for the terminator.
constexpr std::size_t kMaxNameLength = sizeof(DataFileEntryView::name) - 1;

/// Oversized on purpose: the real entry is 0xA8, and a build that grew it must not overrun.
constexpr std::size_t kEntryStorageBytes = 0x100;

/// The table ends at {hash 0, index 0xFFFFFFFF} (FiveM LoadStreamingFile.cpp:886).
constexpr uint32_t kTableEndIndex = 0xFFFFFFFF;

/// Nothing real has thousands of data-file types, so a walk that gets this far is lost.
constexpr std::size_t kMaxTableRows = 1024;

// CDataFileMountInterface vtable.
constexpr std::size_t kSlotLoadDataFile = 1;   ///< bool LoadDataFile(DataFile*)
constexpr std::size_t kSlotUnloadDataFile = 2; ///< void UnloadDataFile(DataFile*)
} // namespace DataFileLayout

namespace MapTypesStoreLayout
{
/// fwMapTypesStore "should async place", before the 2802 shift
/// (StreamingModuleLayout::VtableShift). FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3798 —
/// 29, or 35 on 2802 and later.
constexpr std::size_t kSlotShouldAsyncPlace = 29;
} // namespace MapTypesStoreLayout
} // namespace spl::rage
