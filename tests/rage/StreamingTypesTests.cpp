#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <catch_amalgamated.hpp>

#include "rage/types/FileDeviceTypes.h"
#include "rage/types/StreamingTypes.h"

using namespace spl::rage;

TEST_CASE("StreamingTypes: the manager layout matches the game", "[rage]")
{
    // The header asserts these at compile time as well; repeating them here means a layout
    // change shows up as a failing test rather than only as a build break.
    STATIC_REQUIRE(sizeof(StreamingDataEntry) == 8);
    STATIC_REQUIRE(offsetof(strStreamingInfoManagerView, entries) == 0x000);
    STATIC_REQUIRE(offsetof(strStreamingInfoManagerView, numEntries) == 0x018);
    STATIC_REQUIRE(offsetof(strStreamingInfoManagerView, moduleMgr) == 0x1B8);
    STATIC_REQUIRE(offsetof(strStreamingInfoManagerView, numPendingRequests) == 0x1E0);
    STATIC_REQUIRE(offsetof(strStreamingModuleMgrView, modules) == 0x018);
    STATIC_REQUIRE(offsetof(atPoolView, size) == 0x010);
}

TEST_CASE("StreamingTypes: build 2802 shifts the module vtable by six slots", "[rage]")
{
    using namespace spl::rage::StreamingModuleLayout;

    CHECK(VtableShift(2545) == 0);
    CHECK(VtableShift(2801) == 0);
    CHECK(VtableShift(2802) == 6);
    CHECK(VtableShift(3258) == 6);

    // FindSlot is what verification calls, so its shifted slot is the one that matters.
    CHECK(kSlotFindSlot + VtableShift(2699) == 2);
    CHECK(kSlotFindSlot + VtableShift(3258) == 8);
}

TEST_CASE("StreamingTypes: index types do not convert into each other", "[rage]")
{
    STATIC_REQUIRE_FALSE(std::is_convertible_v<GlobalIndex, LocalSlot>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<LocalSlot, uint32_t>);

    CHECK(LocalSlot{7} == LocalSlot{7});
    CHECK(GlobalIndex{2} < GlobalIndex{3});
    CHECK(StreamingHandle{(1u << 16) | 5u}.value == 0x10005);
}

TEST_CASE("StreamingTypes: the low two flag bits are the load state", "[rage]")
{
    CHECK(LoadStateOf(StreamingDataEntry{.handle = 1, .flags = 0}) == LoadState::NotLoaded);
    CHECK(LoadStateOf(StreamingDataEntry{.handle = 1, .flags = 1}) == LoadState::Loaded);
    CHECK(LoadStateOf(StreamingDataEntry{.handle = 1, .flags = 0x40000002}) ==
          LoadState::Requested);
    CHECK(LoadStateOf(StreamingDataEntry{.handle = 1, .flags = 0xFFFF}) == LoadState::Loading);
}

TEST_CASE("StreamingTypes: a raw handle is the entry index in collection zero", "[rage]")
{
    const StreamingHandle handle = MakeRawHandle(0x158);
    CHECK(handle.value == 0x158);
    CHECK(IsRawHandle(handle));
    CHECK(EntryIndexOf(handle) == 0x158);
    CHECK_FALSE(IsRawHandle(StreamingHandle{(3u << 16) | 0x158u}));
}

TEST_CASE("StreamingTypes: the raw streamer slots follow the fiDevice ones", "[rage]")
{
    // fiCollectionWrapper.h: CloseCollection, OpenCollectionEntry, GetEntry, Unk1, GetEntryName,
    // GetEntryNameToBuffer, GetEntryByName.
    STATIC_REQUIRE(CollectionLayout::kSlotGetEntryName == 51);
    STATIC_REQUIRE(CollectionLayout::kSlotGetEntryByName == 53);
    STATIC_REQUIRE((StreamingModuleLayout::kAssetFlagsClearedToRelease &
                    StreamingModuleLayout::kAssetFlagPermanent) != 0);
}

TEST_CASE("StreamingTypes: a raw entry keeps its path at +0x18", "[rage]")
{
    STATIC_REQUIRE(offsetof(RawCollectionEntryView, fileName) == 0x18);
    STATIC_REQUIRE(sizeof(RawCollectionEntryView) == 0x20);
    STATIC_REQUIRE(CollectionLayout::kFallbackEntriesOffset == 0x5B0);
    STATIC_REQUIRE(CollectionLayout::kEntryCountOffset == 0x200);
}

TEST_CASE("StreamingTypes: the entry list offset is read from the chunk load", "[rage]")
{
    // RDR3's GetEntryNameToBuffer, as FiveM matches it: ... shr rax, 0Ah;
    // mov rax, [rcx + rax*8 + 5B0h].
    constexpr std::array<uint8_t, 21> rdr3 = {0x4D, 0x63, 0xC1, 0x81, 0xE2, 0xFF, 0x03,
                                              0x00, 0x00, 0x48, 0xC1, 0xE8, 0x0A, 0x48,
                                              0x8B, 0x84, 0xC1, 0xB0, 0x05, 0x00, 0x00};
    CHECK(CollectionLayout::FindEntriesOffset(rdr3) == 0x5B0u);

    // Any register, any index: mov r9, [rcx + r8*8 + 5C0h].
    constexpr std::array<uint8_t, 8> otherRegisters = {0x4E, 0x8B, 0x8C, 0xC1,
                                                       0xC0, 0x05, 0x00, 0x00};
    CHECK(CollectionLayout::FindEntriesOffset(otherRegisters) == 0x5C0u);
}

TEST_CASE("StreamingTypes: code without one clear chunk load gives no offset", "[rage]")
{
    constexpr std::array<uint8_t, 13> noLoad = {0x4D, 0x63, 0xC1, 0x41, 0x8B, 0xC2, 0x41,
                                                0x81, 0xE2, 0xFF, 0x03, 0x00, 0x00};
    CHECK_FALSE(CollectionLayout::FindEntriesOffset(noLoad).has_value());

    // base rdx instead of rcx, and scale 4: not the entry list.
    constexpr std::array<uint8_t, 16> wrongShape = {0x48, 0x8B, 0x84, 0xC2, 0xB0, 0x05, 0x00, 0x00,
                                                    0x48, 0x8B, 0x84, 0x81, 0xB0, 0x05, 0x00, 0x00};
    CHECK_FALSE(CollectionLayout::FindEntriesOffset(wrongShape).has_value());

    constexpr std::array<uint8_t, 16> twoOffsets = {0x48, 0x8B, 0x84, 0xC1, 0xB0, 0x05, 0x00, 0x00,
                                                    0x48, 0x8B, 0x84, 0xC1, 0xC0, 0x05, 0x00, 0x00};
    CHECK_FALSE(CollectionLayout::FindEntriesOffset(twoOffsets).has_value());
}
