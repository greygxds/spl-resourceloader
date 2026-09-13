#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "memory/Pattern.h"

using namespace spl::memory;

TEST_CASE("Pattern: parses bytes and wildcards", "[memory]")
{
    const std::optional<Pattern> pattern = Pattern::Parse("48 8B 05 ? ? ?? ? 48 8B CB");
    REQUIRE(pattern.has_value());

    CHECK(pattern->GetLengthBytes() == 10);
    CHECK(pattern->GetBytes()[0] == 0x48);
    CHECK(pattern->GetBytes()[2] == 0x05);
    CHECK(pattern->GetMask()[2] == 0xFF);
    for (size_t index = 3; index <= 6; ++index)
    {
        CHECK(pattern->GetMask()[index] == 0x00);
    }
    CHECK(pattern->GetMask()[7] == 0xFF);
}

TEST_CASE("Pattern: hex is case-insensitive and spacing is free", "[memory]")
{
    const std::optional<Pattern> lower = Pattern::Parse("48 8b cb");
    const std::optional<Pattern> upper = Pattern::Parse("48\t8B\nCB  ");
    REQUIRE(lower.has_value());
    REQUIRE(upper.has_value());

    CHECK(std::vector<uint8_t>(lower->GetBytes().begin(), lower->GetBytes().end()) ==
          std::vector<uint8_t>(upper->GetBytes().begin(), upper->GetBytes().end()));
}

TEST_CASE("Pattern: the anchor is the longest run of fixed bytes", "[memory]")
{
    const std::optional<Pattern> pattern = Pattern::Parse("48 ? 8B 05 E8 ? ? 33 C0");
    REQUIRE(pattern.has_value());

    CHECK(pattern->GetAnchor().offset == 2);
    CHECK(pattern->GetAnchor().lengthBytes == 3);
}

TEST_CASE("Pattern: a leading wildcard still anchors on the fixed tail", "[memory]")
{
    const std::optional<Pattern> pattern = Pattern::Parse("? ? 48 8B");
    REQUIRE(pattern.has_value());

    CHECK(pattern->GetAnchor().offset == 2);
    CHECK(pattern->GetAnchor().lengthBytes == 2);
}

TEST_CASE("Pattern: malformed text is rejected", "[memory]")
{
    CHECK_FALSE(Pattern::Parse("").has_value());
    CHECK_FALSE(Pattern::Parse("   ").has_value());
    CHECK_FALSE(Pattern::Parse("? ? ?").has_value()); // would match everywhere
    CHECK_FALSE(Pattern::Parse("48 ZZ").has_value());
    CHECK_FALSE(Pattern::Parse("48 8B;").has_value());
}

TEST_CASE("Pattern: MatchesAt honours the mask", "[memory]")
{
    const std::optional<Pattern> pattern = Pattern::Parse("48 ? CB");
    REQUIRE(pattern.has_value());

    const std::vector<uint8_t> matching = {0x48, 0x00, 0xCB};
    const std::vector<uint8_t> wildcardDiffers = {0x48, 0xFF, 0xCB};
    const std::vector<uint8_t> fixedDiffers = {0x48, 0x00, 0xCC};

    CHECK(pattern->MatchesAt(matching.data()));
    CHECK(pattern->MatchesAt(wildcardDiffers.data()));
    CHECK_FALSE(pattern->MatchesAt(fixedDiffers.data()));
}

TEST_CASE("Pattern: the text is kept for the scan cache and logs", "[memory]")
{
    const std::optional<Pattern> pattern = Pattern::Parse("48 8B 05");
    REQUIRE(pattern.has_value());

    CHECK(pattern->GetText() == "48 8B 05");
}
