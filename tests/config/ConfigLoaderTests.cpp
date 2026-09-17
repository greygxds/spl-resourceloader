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

using spl::config::ConfigLoader;
using spl::config::ConfigLoadResult;
using spl::config::DuplicatePolicy;
using spl::config::LoaderConfig;
using spl::config::LogLevel;
using spl::config::MapReloadStrategy;
using spl::config::SafeMode;
using spl::config::StreamingSettings;

namespace
{
/// True when any diagnostic mentions text, which is how these tests assert that the message
/// names the offending key rather than just counting warnings.
bool Mentions(const std::vector<std::string>& messages, std::string_view text)
{
    return std::ranges::any_of(messages, [&](const std::string& message)
                               { return message.find(text) != std::string::npos; });
}
} // namespace

TEST_CASE("ConfigLoader: the default text parses to the default config", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse(spl::config::DefaultConfigText());

    INFO("warnings: " << fmt::format("{}", fmt::join(result.diagnostics.warnings, "; ")));
    REQUIRE(result.diagnostics.warnings.empty());
    REQUIRE(result.diagnostics.errors.empty());
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: an empty document keeps every default", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("");

    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: every level name maps to its level", "[config]")
{
    struct Case
    {
        std::string_view text;
        LogLevel expected;
    };
    const auto testCase =
        GENERATE(Case{"trace", LogLevel::Trace}, Case{"debug", LogLevel::Debug},
                 Case{"info", LogLevel::Info}, Case{"warning", LogLevel::Warning},
                 Case{"error", LogLevel::Error}, Case{"critical", LogLevel::Critical},
                 Case{"off", LogLevel::Off});

    const ConfigLoadResult result =
        ConfigLoader::Parse(fmt::format("[logging]\nlevel = \"{}\"\n", testCase.text));

    INFO("level = " << testCase.text);
    REQUIRE(result.diagnostics.warnings.empty());
    REQUIRE(result.config.logging.level == testCase.expected);
}

TEST_CASE("ConfigLoader: level names are case-insensitive", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[logging]\nlevel = \"  DeBuG \"\n");

    REQUIRE(result.diagnostics.warnings.empty());
    REQUIRE(result.config.logging.level == LogLevel::Debug);
}

TEST_CASE("ConfigLoader: level aliases work but are reported", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[logging]\nlevel = \"warn\"\n");

    REQUIRE(result.config.logging.level == LogLevel::Warning);
    REQUIRE(Mentions(result.diagnostics.warnings, "prefer 'warning'"));
}

TEST_CASE("ConfigLoader: an unknown level warns and keeps the default", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[logging]\nlevel = \"verbose\"\n");

    REQUIRE(result.config.logging.level == LogLevel::Warning);
    REQUIRE(Mentions(result.diagnostics.warnings, "verbose"));
}

TEST_CASE("ConfigLoader: an unknown key warns and is ignored", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[streaming]\nload_mapz = true\n");

    REQUIRE(Mentions(result.diagnostics.warnings, "streaming.load_mapz"));
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: an unknown table warns and is ignored", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[networking]\nenabled = true\n");

    REQUIRE(Mentions(result.diagnostics.warnings, "networking"));
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: a wrongly typed key warns and keeps the default", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[loader]\nenabled = \"yes\"\n");

    REQUIRE(result.config.loader.enabled);
    REQUIRE(Mentions(result.diagnostics.warnings, "loader.enabled"));
}

TEST_CASE("ConfigLoader: an out-of-range integer warns and keeps the default", "[config]")
{
    const ConfigLoadResult result =
        ConfigLoader::Parse("[diagnostics]\nasset_size_warning_mib = -5\n");

    REQUIRE(result.config.diagnostics.assetSizeWarningMiB == 256);
    REQUIRE(Mentions(result.diagnostics.warnings, "asset_size_warning_mib"));
}

TEST_CASE("ConfigLoader: the memory table is read and defaults to off", "[config]")
{
    const LoaderConfig defaults = ConfigLoader::Parse("").config;
    REQUIRE_FALSE(defaults.memory.extendedTextureBudget);
    REQUIRE(defaults.memory.textureBudgetScale == 0);
    REQUIRE_FALSE(defaults.memory.extendedStreamingMemory);

    const ConfigLoadResult result =
        ConfigLoader::Parse("[memory]\nextended_texture_budget = true\ntexture_budget_scale = 12\n"
                            "extended_streaming_memory = true\n");
    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE(result.config.memory.extendedTextureBudget);
    REQUIRE(result.config.memory.textureBudgetScale == 12);
    REQUIRE(result.config.memory.extendedStreamingMemory);
}

TEST_CASE("ConfigLoader: a texture budget scale above 12 warns and keeps the default", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[memory]\ntexture_budget_scale = 13\n");

    REQUIRE(result.config.memory.textureBudgetScale == 0);
    REQUIRE(Mentions(result.diagnostics.warnings, "memory.texture_budget_scale"));
}

TEST_CASE("ConfigLoader: auto_request_ytyp is read and defaults to off", "[config]")
{
    REQUIRE_FALSE(ConfigLoader::Parse("").config.streaming.autoRequestYtyp);

    const ConfigLoadResult result = ConfigLoader::Parse("[streaming]\nauto_request_ytyp = true\n");
    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE(result.config.streaming.autoRequestYtyp);
}

TEST_CASE("ConfigLoader: mp_maps defaults to on and deferred to the MP map prefixes", "[config]")
{
    const StreamingSettings defaults = ConfigLoader::Parse("").config.streaming;
    REQUIRE(defaults.mpMaps);
    REQUIRE(defaults.deferred ==
            std::vector<std::string>{"hei_*", "apa_*", "lr_*", "vw_*", "bkr_*"});

    const ConfigLoadResult result =
        ConfigLoader::Parse("[streaming]\nmp_maps = false\ndeferred = [\"xm_*\"]\n");
    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE_FALSE(result.config.streaming.mpMaps);
    REQUIRE(result.config.streaming.deferred == std::vector<std::string>{"xm_*"});
}

TEST_CASE("ConfigLoader: an empty deferred list turns the wait off", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[streaming]\ndeferred = []\n");
    REQUIRE(result.diagnostics.IsEmpty());
    REQUIRE(result.config.streaming.deferred.empty());
}

TEST_CASE("ConfigLoader: the removed dev table is reported as unknown", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[dev]\ndisable_patches = true\n");

    REQUIRE(Mentions(result.diagnostics.warnings, "dev"));
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: mods disabled and priority are read", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse(R"([mods]
disabled = ["OldCar"]
priority = ["mycar", "mymap"]
)");

    REQUIRE(result.diagnostics.IsEmpty());
    CHECK(result.config.mods.disabled == std::vector<std::string>{"OldCar"});
    CHECK(result.config.mods.priority == std::vector<std::string>{"mycar", "mymap"});
}

TEST_CASE("ConfigLoader: duplicate_policy accepts first and last", "[config]")
{
    REQUIRE(ConfigLoader::Parse("[streaming]\nduplicate_policy = \"last\"\n")
                .config.streaming.duplicatePolicy == DuplicatePolicy::LastWins);
    REQUIRE(ConfigLoader::Parse("[streaming]\nduplicate_policy = \"FIRST\"\n")
                .config.streaming.duplicatePolicy == DuplicatePolicy::FirstWins);

    const ConfigLoadResult bad =
        ConfigLoader::Parse("[streaming]\nduplicate_policy = \"newest\"\n");
    REQUIRE(bad.config.streaming.duplicatePolicy == DuplicatePolicy::FirstWins);
    REQUIRE(Mentions(bad.diagnostics.warnings, "newest"));
}

TEST_CASE("ConfigLoader: disabled entries are trimmed and deduped case-insensitively", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse(
        "[resources]\ndisabled = [\" map_one \", \"MAP_ONE\", \"map_two\", \"\"]\n");

    REQUIRE(result.config.resources.disabled == std::vector<std::string>{"map_one", "map_two"});
    // The duplicate is named exactly as the user wrote it, so they can find it in the file.
    REQUIRE(Mentions(result.diagnostics.warnings, "'MAP_ONE' more than once"));
    REQUIRE(Mentions(result.diagnostics.warnings, "resources.disabled"));
    REQUIRE(Mentions(result.diagnostics.warnings, "empty entry"));
}

TEST_CASE("ConfigLoader: priority keeps the order it was written in", "[config]")
{
    const ConfigLoadResult result =
        ConfigLoader::Parse("[resources]\npriority = [\"second\", \"first\"]\n");

    REQUIRE(result.config.resources.priority == std::vector<std::string>{"second", "first"});
    REQUIRE(result.diagnostics.warnings.empty());
}

TEST_CASE("ConfigLoader: [data_files] and load_animations are read", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse(
        "[streaming]\nload_animations = false\n"
        "[data_files]\nenabled = true\nvehicles = false\nweapons = true\npeds = false\n"
        "audio = false\nother = false\ndisabled_types = [\"CARCOLS_FILE\", \"carcols_file\"]\n");

    REQUIRE_FALSE(result.config.streaming.loadAnimations);
    const spl::config::DataFileSettings& dataFiles = result.config.dataFiles;
    CHECK(dataFiles.enabled);
    CHECK_FALSE(dataFiles.vehicles);
    CHECK(dataFiles.weapons);
    CHECK_FALSE(dataFiles.peds);
    CHECK_FALSE(dataFiles.audio);
    CHECK_FALSE(dataFiles.other);
    CHECK(dataFiles.disabledTypes == std::vector<std::string>{"CARCOLS_FILE"});
    CHECK(Mentions(result.diagnostics.warnings, "data_files.disabled_types"));
}

TEST_CASE("ConfigLoader: [mods] is read", "[config]")
{
    const ConfigLoadResult result =
        ConfigLoader::Parse("[paths]\nmods = \"content\"\n[mods]\nenabled = false\n");

    REQUIRE(result.config.paths.mods == std::filesystem::path{"content"});
    REQUIRE_FALSE(result.config.mods.enabled);
    REQUIRE(result.diagnostics.warnings.empty());
}

TEST_CASE("ConfigLoader: a syntax error reports line and column and keeps defaults", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[logging]\nlevel = \n");

    REQUIRE_FALSE(result.diagnostics.errors.empty());
    REQUIRE(Mentions(result.diagnostics.errors, "line 2"));
    REQUIRE(Mentions(result.diagnostics.errors, "column"));
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: a table of the wrong shape is reported, not fatal", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("logging = 3\n");

    REQUIRE(result.diagnostics.errors.empty());
    REQUIRE(Mentions(result.diagnostics.warnings, "logging"));
    REQUIRE(result.config == LoaderConfig{});
}

TEST_CASE("ConfigLoader: ToString round-trips every canonical level", "[config]")
{
    for (const LogLevel level :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Error,
          LogLevel::Critical, LogLevel::Off})
    {
        const std::string text{spl::config::ToString(level)};
        const ConfigLoadResult result =
            ConfigLoader::Parse(fmt::format("[logging]\nlevel = \"{}\"\n", text));

        INFO("level = " << text);
        REQUIRE(result.diagnostics.warnings.empty());
        REQUIRE(result.config.logging.level == level);
    }
}

TEST_CASE("ConfigLoader: map_reload_strategy accepts its three names", "[config]")
{
    CHECK(ConfigLoader::Parse("").config.diagnostics.mapReloadStrategy == MapReloadStrategy::Auto);
    CHECK(ConfigLoader::Parse("[diagnostics]\nmap_reload_strategy = \"change_set_replay\"\n")
              .config.diagnostics.mapReloadStrategy == MapReloadStrategy::ChangeSetReplay);
    CHECK(ConfigLoader::Parse("[diagnostics]\nmap_reload_strategy = \"Content_Group_Toggle\"\n")
              .config.diagnostics.mapReloadStrategy == MapReloadStrategy::ContentGroupToggle);

    const ConfigLoadResult bad =
        ConfigLoader::Parse("[diagnostics]\nmap_reload_strategy = \"fast\"\n");
    CHECK(bad.config.diagnostics.mapReloadStrategy == MapReloadStrategy::Auto);
    CHECK(Mentions(bad.diagnostics.warnings, "fast"));
}

TEST_CASE("ConfigLoader: safe_mode accepts auto, off and a boolean", "[config]")
{
    CHECK(ConfigLoader::Parse("").config.loader.safeMode == SafeMode::Auto);
    CHECK(ConfigLoader::Parse("[loader]\nsafe_mode = \"OFF\"\n").config.loader.safeMode ==
          SafeMode::Off);
    CHECK(ConfigLoader::Parse("[loader]\nsafe_mode = false\n").config.loader.safeMode ==
          SafeMode::Off);
    CHECK(ConfigLoader::Parse("[loader]\nsafe_mode = true\n").config.loader.safeMode ==
          SafeMode::Auto);

    const ConfigLoadResult bad = ConfigLoader::Parse("[loader]\nsafe_mode = \"always\"\n");
    CHECK(bad.config.loader.safeMode == SafeMode::Auto);
    CHECK(Mentions(bad.diagnostics.warnings, "always"));
}

TEST_CASE("ConfigLoader: write_minidump is read", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[diagnostics]\nwrite_minidump = true\n");

    REQUIRE(result.diagnostics.IsEmpty());
    CHECK(result.config.diagnostics.writeMinidump);
    CHECK_FALSE(LoaderConfig{}.diagnostics.writeMinidump);
}

TEST_CASE("ConfigLoader: resources.enabled is read and defaults to on", "[config]")
{
    CHECK(LoaderConfig{}.resources.enabled);

    const ConfigLoadResult result = ConfigLoader::Parse("[resources]\nenabled = false\n");
    REQUIRE(result.diagnostics.IsEmpty());
    CHECK_FALSE(result.config.resources.enabled);
}

TEST_CASE("ConfigLoader: early_init is read and on by default", "[config]")
{
    const ConfigLoadResult result = ConfigLoader::Parse("[loader]\nearly_init = false\n");

    REQUIRE(result.diagnostics.IsEmpty());
    CHECK_FALSE(result.config.loader.earlyInit);
    CHECK(LoaderConfig{}.loader.earlyInit);
}
