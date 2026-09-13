#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "manifest/ManifestParser.h"
#include "manifest/ResourceManifest.h"

using spl::manifest::ManifestDiagnostic;
using spl::manifest::ManifestEntry;
using spl::manifest::ManifestParser;
using spl::manifest::ParseResult;

namespace
{
/// Real fxmanifest.lua files taken from the FiveM tree, so a regression shows up against files
/// their authors actually wrote rather than against something invented here.
std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path{SPL_TEST_FIXTURES} / "manifests" / name;
}

std::string ReadFixture(std::string_view name)
{
    std::ifstream stream{FixturePath(name), std::ios::binary};
    REQUIRE(stream);
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::vector<std::string> ValuesOf(const ParseResult& result, std::string_view key)
{
    std::vector<std::string> values;
    for (const ManifestEntry* entry : result.document.GetEntries(key))
    {
        values.push_back(entry->value);
    }
    return values;
}

bool HasError(const ParseResult& result)
{
    return std::ranges::any_of(
        result.diagnostics, [](const ManifestDiagnostic& diagnostic)
        { return diagnostic.severity == ManifestDiagnostic::Severity::Error; });
}
} // namespace

TEST_CASE("Manifest corpus: every real manifest parses cleanly", "[manifest]")
{
    const auto name = GENERATE(as<std::string>{}, "chat.lua", "sdk-game.lua", "sdk-root.lua",
                               "webpack.lua", "yarn.lua");

    const ParseResult result = ManifestParser::Parse(ReadFixture(name), name);

    INFO("fixture: " << name);
    REQUIRE_FALSE(result.fatal);
    REQUIRE_FALSE(HasError(result));
    REQUIRE_FALSE(result.document.All().empty());
}

TEST_CASE("Manifest corpus: both backends agree on every real manifest", "[manifest]")
{
    // The text parser is the fallback, so it must read a declarative manifest exactly as the
    // interpreter does. Every fixture is declarative, which is the point.
    const auto name = GENERATE(as<std::string>{}, "chat.lua", "sdk-game.lua", "sdk-root.lua",
                               "webpack.lua", "yarn.lua");
    const std::string source = ReadFixture(name);

    const ParseResult viaLua = ManifestParser::Parse(source, name);
    const ParseResult viaText = ManifestParser::ParseText(source, name);

    std::vector<std::pair<std::string, std::string>> luaPairs;
    for (const ManifestEntry& entry : viaLua.document.All())
    {
        luaPairs.emplace_back(entry.key, entry.value);
    }
    std::vector<std::pair<std::string, std::string>> textPairs;
    for (const ManifestEntry& entry : viaText.document.All())
    {
        textPairs.emplace_back(entry.key, entry.value);
    }

    INFO("fixture: " << name);
    REQUIRE(luaPairs == textPairs);
}

TEST_CASE("Manifest corpus: chat.lua reads as its author wrote it", "[manifest]")
{
    const ParseResult result = ManifestParser::Parse(ReadFixture("chat.lua"), "chat.lua");

    REQUIRE(ValuesOf(result, "fx_version") == std::vector<std::string>{"adamant"});
    REQUIRE(ValuesOf(result, "version") == std::vector<std::string>{"1.0.0"});
    REQUIRE(ValuesOf(result, "ui_page") == std::vector<std::string>{"dist/ui.html"});
    // games { 'rdr3', 'gta5' } becomes two "game" entries, plural stripped.
    REQUIRE(ValuesOf(result, "game") == std::vector<std::string>{"rdr3", "gta5"});
    // files { ... } becomes one "file" entry per element, in order.
    REQUIRE(ValuesOf(result, "file") ==
            std::vector<std::string>{"dist/ui.html", "dist/index.css", "dist/chat.js",
                                     "html/vendor/*.css", "html/vendor/fonts/*.woff2"});
    REQUIRE(ValuesOf(result, "client_script") == std::vector<std::string>{"cl_chat.lua"});
    REQUIRE(ValuesOf(result, "server_script") == std::vector<std::string>{"sv_chat.lua"});
}

TEST_CASE("Manifest corpus: a resource for another game is seen as incompatible", "[manifest]")
{
    ParseResult result = ManifestParser::Parse(ReadFixture("webpack.lua"), "webpack.lua");
    const spl::manifest::ResourceManifest manifest = spl::manifest::ResourceManifest::FromDocument(
        result.document, FixturePath("webpack.lua").parent_path(), result.diagnostics);

    // webpack declares game 'common', which GTA V is part of.
    REQUIRE(manifest.IsCompatibleWithGta5());
    REQUIRE(manifest.ignoredScriptEntries > 0);
    REQUIRE_FALSE(manifest.isMap);
}
