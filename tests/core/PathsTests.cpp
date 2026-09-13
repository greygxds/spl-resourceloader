#include <filesystem>

#include <catch_amalgamated.hpp>

#include "core/Paths.h"

TEST_CASE("Paths: derives the data folder from the game directory", "[core]")
{
    const std::filesystem::path gameDir = "C:/Games/GTAV";
    const spl::Paths paths = spl::Paths::FromGameDir(gameDir);

    REQUIRE(paths.gameDir == gameDir);
    REQUIRE(paths.dataDir == gameDir / "resourceLoader");
    REQUIRE(paths.configFile == gameDir / "resourceLoader" / "config.toml");
    REQUIRE(paths.stateFile.parent_path() == paths.dataDir);
    REQUIRE(paths.markerFile.parent_path() == paths.dataDir);
    REQUIRE(paths.crashReportFile == gameDir / "resourceLoader" / "crash.txt");
}

TEST_CASE("Paths: resolves against the running executable", "[core]")
{
    const auto paths = spl::Paths::Resolve();

    REQUIRE(paths.has_value());
    REQUIRE(paths->dataDir.parent_path() == paths->gameDir);
    REQUIRE(std::filesystem::exists(paths->gameDir));
}

TEST_CASE("Paths: a relative user path resolves against the data folder", "[core]")
{
    const spl::Paths paths = spl::Paths::FromGameDir("C:/Games/GTAV");

    REQUIRE(paths.ResolveUserPath("resources") ==
            std::filesystem::path{"C:/Games/GTAV/resourceLoader/resources"});
    REQUIRE(paths.ResolveUserPath("resourceLoader.log") ==
            std::filesystem::path{"C:/Games/GTAV/resourceLoader/resourceLoader.log"});
}

TEST_CASE("Paths: an absolute user path is used as it is", "[core]")
{
    const spl::Paths paths = spl::Paths::FromGameDir("C:/Games/GTAV");

    REQUIRE(paths.ResolveUserPath("D:/mods/assets") == std::filesystem::path{"D:/mods/assets"});
}

TEST_CASE("Paths: a user path is normalized", "[core]")
{
    const spl::Paths paths = spl::Paths::FromGameDir("C:/Games/GTAV");

    REQUIRE(paths.ResolveUserPath("sub/../resources") ==
            std::filesystem::path{"C:/Games/GTAV/resourceLoader/resources"});
}
