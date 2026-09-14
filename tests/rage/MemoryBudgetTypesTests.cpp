#include <cstdint>

#include <catch_amalgamated.hpp>

#include "rage/types/MemoryBudgetTypes.h"

using namespace spl::rage;
using namespace spl::rage::MemoryBudgetLayout;

TEST_CASE("MemoryBudgetTypes: scale 0 gives FiveM's 3 GB budget", "[rage]")
{
    const auto row = TextureBudgetRow(0);

    CHECK(row[3] == kBaseTextureBudgetBytes);
    CHECK(row[2] == kBaseTextureBudgetBytes);
    CHECK(row[1] == kBaseTextureBudgetBytes * 2 / 3);
    CHECK(row[0] == kBaseTextureBudgetBytes / 2);
}

TEST_CASE("MemoryBudgetTypes: scale 12 doubles the budget and higher scales are capped", "[rage]")
{
    CHECK(TextureBudgetRow(12)[3] == 2 * kBaseTextureBudgetBytes);
    CHECK(TextureBudgetRow(6)[3] == kBaseTextureBudgetBytes * 3 / 2);
    CHECK(TextureBudgetRow(100) == TextureBudgetRow(12));
}

TEST_CASE("MemoryBudgetTypes: the streaming allocator grows only with 12 GB of RAM", "[rage]")
{
    CHECK_FALSE(StreamingAllocatorBytesFor(8 * kGigabyteBytes));
    CHECK_FALSE(StreamingAllocatorBytesFor(kLargeSystemMemoryBytes - 1));
    CHECK(StreamingAllocatorBytesFor(kLargeSystemMemoryBytes) == kLargeStreamingAllocatorBytes);
    CHECK(StreamingAllocatorBytesFor(kHugeSystemMemoryBytes - 1) == kLargeStreamingAllocatorBytes);
    CHECK(StreamingAllocatorBytesFor(kHugeSystemMemoryBytes) == kHugeStreamingAllocatorBytes);
    CHECK(StreamingAllocatorBytesFor(64 * kGigabyteBytes) == kHugeStreamingAllocatorBytes);
}
