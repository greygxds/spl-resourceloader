#include <string>

#include <catch_amalgamated.hpp>

#include "core/Version.h"

TEST_CASE("Version: describes itself with a build and a commit", "[core]")
{
    const std::string described = spl::Version::Describe();

    REQUIRE_FALSE(spl::Version::kText.empty());
    REQUIRE_FALSE(spl::Version::kCommit.empty());
    REQUIRE(described.find(spl::Version::kText) != std::string::npos);
    REQUIRE(described.find(spl::Version::kCommit) != std::string::npos);
}

TEST_CASE("Version: reports the build configuration", "[core]")
{
    const std::string_view configuration = spl::Version::Configuration();

    REQUIRE((configuration == "Debug" || configuration == "Dev" || configuration == "Release"));
}
