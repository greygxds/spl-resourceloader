#include <filesystem>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "config/ConfigLoader.h"
#include "config/DefaultConfig.h"
#include "config/LoaderConfig.h"
#include "tests/TempTree.h"

using spl::config::ConfigLoader;
using spl::config::ConfigLoadResult;
using spl::config::LoaderConfig;
using spl::config::LogLevel;
using spl::tests::TempDir;

TEST_CASE("ConfigLoader: a missing file is created with the default text", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";

    const ConfigLoadResult result = ConfigLoader::LoadOrCreate(file);

    REQUIRE(result.wroteDefault);
    REQUIRE(std::filesystem::exists(file));
    REQUIRE(temp.ReadFile("config.toml") == spl::config::DefaultConfigText());
    REQUIRE(result.config == LoaderConfig{});
    REQUIRE(result.diagnostics.IsEmpty());
}

TEST_CASE("ConfigLoader: an existing file is read, not rewritten", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";
    temp.WriteFile("config.toml", "[logging]\nlevel = \"debug\"\n");

    const ConfigLoadResult result = ConfigLoader::LoadOrCreate(file);

    REQUIRE_FALSE(result.wroteDefault);
    REQUIRE(result.config.logging.level == LogLevel::Debug);
    REQUIRE(temp.ReadFile("config.toml") == "[logging]\nlevel = \"debug\"\n");
}

TEST_CASE("ConfigLoader: a broken file is never overwritten", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";
    constexpr std::string_view broken = "[logging\nlevel = \"debug\"\n";
    temp.WriteFile("config.toml", broken);

    const ConfigLoadResult result = ConfigLoader::LoadOrCreate(file);

    REQUIRE_FALSE(result.diagnostics.errors.empty());
    REQUIRE(result.config == LoaderConfig{});
    REQUIRE(temp.ReadFile("config.toml") == broken);
}

TEST_CASE("ConfigLoader: the created file parses back to the defaults", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";

    REQUIRE(ConfigLoader::LoadOrCreate(file).wroteDefault);
    const ConfigLoadResult reread = ConfigLoader::LoadOrCreate(file);

    REQUIRE_FALSE(reread.wroteDefault);
    REQUIRE(reread.diagnostics.IsEmpty());
    REQUIRE(reread.config == LoaderConfig{});
}
