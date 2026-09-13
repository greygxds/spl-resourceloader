#include <string>

#include <catch_amalgamated.hpp>

#include "util/Strings.h"

TEST_CASE("Strings: ToLower lowers ASCII and leaves the rest alone", "[util]")
{
    REQUIRE(spl::util::ToLower("MAP_One.YDR") == "map_one.ydr");
    REQUIRE(spl::util::ToLower("") == "");
    REQUIRE(spl::util::ToLower("123-_.") == "123-_.");
}

TEST_CASE("Strings: Trim removes surrounding whitespace only", "[util]")
{
    REQUIRE(spl::util::Trim("  map one \t\r\n") == "map one");
    REQUIRE(spl::util::Trim("map") == "map");
    REQUIRE(spl::util::Trim("   ") == "");
    REQUIRE(spl::util::Trim("") == "");
}

TEST_CASE("Strings: EqualsIgnoreCase compares without case", "[util]")
{
    REQUIRE(spl::util::EqualsIgnoreCase("Map_One", "MAP_ONE"));
    REQUIRE(spl::util::EqualsIgnoreCase("", ""));
    REQUIRE_FALSE(spl::util::EqualsIgnoreCase("map", "map2"));
    REQUIRE_FALSE(spl::util::EqualsIgnoreCase("map", "pam"));
}
