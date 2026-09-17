#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>

#include "config/ConfigLoader.h"
#include "config/DefaultConfig.h"
#include "config/LoaderConfig.h"
#include "tests/TempTree.h"

using spl::config::ConfigLoader;
using spl::config::ConfigLoadResult;
using spl::config::LoaderConfig;
using spl::config::LogLevel;
using spl::tests::TempDir;

namespace
{
/// The default text with one value changed, the way a user's file looks: complete, and theirs.
std::string CompleteUserConfig()
{
    std::string text{spl::config::DefaultConfigText()};
    const std::string from = "level = \"warning\"";
    text.replace(text.find(from), from.size(), "level = \"debug\"");
    return text;
}
} // namespace

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
    REQUIRE(result.addedOptions.empty());
}

TEST_CASE("ConfigLoader: a complete existing file is read, not rewritten", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";
    const std::string complete = CompleteUserConfig();
    temp.WriteFile("config.toml", complete);

    const ConfigLoadResult result = ConfigLoader::LoadOrCreate(file);

    REQUIRE_FALSE(result.wroteDefault);
    REQUIRE(result.addedOptions.empty());
    REQUIRE(result.config.logging.level == LogLevel::Debug);
    REQUIRE(temp.ReadFile("config.toml") == complete);
    REQUIRE_FALSE(std::filesystem::exists(temp.Path() / "config.toml.bak"));
}

TEST_CASE("ConfigLoader: an older file gains the new options and keeps a backup", "[config]")
{
    const TempDir temp;
    const std::filesystem::path file = temp.Path() / "config.toml";
    constexpr std::string_view older = "[logging]\nlevel = \"debug\"\n";
    temp.WriteFile("config.toml", older);

    const ConfigLoadResult result = ConfigLoader::LoadOrCreate(file);

    INFO("warnings: " << fmt::format("{}", fmt::join(result.diagnostics.warnings, "; ")));
    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE(result.config.logging.level == LogLevel::Debug);
    REQUIRE(std::ranges::find(result.addedOptions, "streaming.mp_maps") !=
            result.addedOptions.end());
    REQUIRE(temp.ReadFile("config.toml.bak") == older);

    // The new file keeps the user's value and reads back without anything left to add.
    const ConfigLoadResult reread = ConfigLoader::LoadOrCreate(file);
    REQUIRE(reread.diagnostics.IsEmpty());
    REQUIRE(reread.addedOptions.empty());
    REQUIRE(reread.config.logging.level == LogLevel::Debug);
    LoaderConfig expected;
    expected.logging.level = LogLevel::Debug;
    REQUIRE(reread.config == expected);
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
    REQUIRE(result.addedOptions.empty());
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
    REQUIRE(reread.addedOptions.empty());
    REQUIRE(reread.config == LoaderConfig{});
}
