#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "logging/AsciiArt.h"
#include "logging/Logger.h"

TEST_CASE("SplitBannerLines: the banner has six non-empty lines", "[logging]")
{
    const std::vector<std::string> lines = spl::logging::SplitBannerLines(spl::logging::kAsciiArt);

    REQUIRE(lines.size() == 6);
    for (const std::string& line : lines)
    {
        REQUIRE_FALSE(line.empty());
    }
}

TEST_CASE("SplitBannerLines: strips CR from CRLF input", "[logging]")
{
    const std::vector<std::string> lines =
        spl::logging::SplitBannerLines("\r\nfirst\r\nsecond\r\n");

    REQUIRE(lines == std::vector<std::string>{"first", "second"});
}

TEST_CASE("SplitBannerLines: keeps a single line without newlines", "[logging]")
{
    REQUIRE(spl::logging::SplitBannerLines("only") == std::vector<std::string>{"only"});
}

TEST_CASE("BannerText: two empty lines above and below the art", "[logging]")
{
    const std::string banner = spl::logging::BannerText();
    const std::vector<std::string> art = spl::logging::SplitBannerLines(spl::logging::kAsciiArt);

    REQUIRE(banner.starts_with("\n\n" + art.front() + "\n"));
    REQUIRE(banner.ends_with(art.back() + "\n\n\n" + spl::logging::VersionLine()));
}
