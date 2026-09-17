#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rage/types/MapStoreTypes.h"
#include "util/Hash.h"

using namespace spl::rage;
using namespace spl::rage::ChangeSetReplayLayout;

TEST_CASE("MapStoreTypes: the replay sites stay inside the function and never overlap", "[rage]")
{
    std::vector<bool> covered(kFunctionBytes, false);
    for (const Site& site : kSites)
    {
        REQUIRE(site.offset >= 0);
        REQUIRE(static_cast<std::size_t>(site.offset) + site.sizeBytes <= kFunctionBytes);
        for (std::size_t index = 0; index < site.sizeBytes; ++index)
        {
            CHECK_FALSE(covered[site.offset + index]);
            covered[site.offset + index] = true;
        }
        CHECK(BuildSiteBytes(site).size() == site.sizeBytes);
    }
}

TEST_CASE("MapStoreTypes: the replay writes the bytes FiveM's ReloadMapStoreNative writes",
          "[rage]")
{
    std::vector<uint8_t> function(kFunctionBytes, 0xCC);
    for (const Site& site : kSites)
    {
        const std::vector<uint8_t> bytes = BuildSiteBytes(site);
        std::ranges::copy(bytes, function.begin() + site.offset);
    }

    // hook::put<uint8_t>(+0x41, 0xE9); hook::put<int32_t>(+0x42, 0x116)
    CHECK(function[0x41] == 0xE9);
    CHECK(function[0x42] == 0x16);
    CHECK(function[0x43] == 0x01);
    CHECK(function[0x44] == 0x00);
    CHECK(function[0x45] == 0x00);

    // hook::nop(+0x356, 10); hook::put<uint16_t>(+0x356, 0x00B3)
    CHECK(function[0x356] == 0xB3);
    CHECK(function[0x357] == 0x00);
    CHECK(std::all_of(function.begin() + 0x358, function.begin() + 0x360,
                      [](uint8_t byte) { return byte == 0x90; }));

    for (const std::ptrdiff_t offset : {0x28, 0x300, 0x395, 0x434, 0x489})
    {
        CHECK(std::all_of(function.begin() + offset, function.begin() + offset + 5,
                          [](uint8_t byte) { return byte == 0x90; }));
    }
    CHECK(std::all_of(function.begin() + 0x4A3, function.begin() + 0x4A3 + 54,
                      [](uint8_t byte) { return byte == 0x90; }));

    // Nothing else is touched.
    CHECK(function[0x27] == 0xCC);
    CHECK(function[0x2D] == 0xCC);
    CHECK(function[kFunctionBytes - 1] == 0xCC);
}

TEST_CASE("MapStoreTypes: the story map group hashes to what the game pushes", "[rage]")
{
    // FiveM EnableMPMapData.cpp:30 patches "mov edx, 0x578F99E2" at the game's own call site.
    CHECK(spl::util::JoaatLower(ContentGroupLayout::kStoryMapGroup) == 0x578F99E2);
}

TEST_CASE("MapStoreTypes: the multiplayer map group hashes to what FiveM finds", "[rage]")
{
    // FiveM LoadStreamingFile.cpp:3731 anchors on "79 91 C8 BC", the GROUP_MAP immediate.
    CHECK(spl::util::JoaatLower(ContentGroupLayout::kMultiplayerMapGroup) == 0xBCC89179);
}
