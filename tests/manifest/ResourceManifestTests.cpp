#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "manifest/ManifestParser.h"
#include "manifest/ResourceManifest.h"
#include "tests/TempTree.h"

using spl::manifest::ManifestDiagnostic;
using spl::manifest::ManifestParser;
using spl::manifest::ResourceManifest;
using spl::tests::TempDir;

namespace
{
struct Interpreted
{
    ResourceManifest manifest;
    std::vector<ManifestDiagnostic> diagnostics;

    [[nodiscard]] bool HasError() const
    {
        return std::ranges::any_of(
            diagnostics, [](const ManifestDiagnostic& diagnostic)
            { return diagnostic.severity == ManifestDiagnostic::Severity::Error; });
    }

    [[nodiscard]] bool Mentions(std::string_view text) const
    {
        return std::ranges::any_of(diagnostics, [&](const ManifestDiagnostic& diagnostic)
                                   { return diagnostic.message.find(text) != std::string::npos; });
    }
};

Interpreted Interpret(std::string_view source, const std::filesystem::path& root)
{
    Interpreted result;
    spl::manifest::ParseResult parsed = ManifestParser::Parse(source, "fxmanifest.lua");
    result.diagnostics = std::move(parsed.diagnostics);
    result.manifest = ResourceManifest::FromDocument(parsed.document, root, result.diagnostics);
    return result;
}
} // namespace

TEST_CASE("ResourceManifest: reads the informational fields", "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret(R"(
fx_version 'cerulean'
name 'Test'
author 'Someone'
description 'A map'
version '1.2.3'
)",
                                         dir.Path());

    REQUIRE(result.manifest.fxVersion == "cerulean");
    REQUIRE(result.manifest.name == "Test");
    REQUIRE(result.manifest.author == "Someone");
    REQUIRE(result.manifest.description == "A map");
    REQUIRE(result.manifest.version == "1.2.3");
}

TEST_CASE("ResourceManifest: this_is_a_map is a flag, whatever its value", "[manifest]")
{
    const TempDir dir;

    REQUIRE(Interpret("this_is_a_map 'yes'", dir.Path()).manifest.isMap);
    REQUIRE(Interpret("this_is_a_map 'no'", dir.Path()).manifest.isMap);
    REQUIRE_FALSE(Interpret("game 'gta5'", dir.Path()).manifest.isMap);
}

TEST_CASE("ResourceManifest: game compatibility follows the games list", "[manifest]")
{
    const TempDir dir;

    REQUIRE(Interpret("game 'gta5'", dir.Path()).manifest.IsCompatibleWithGta5());
    REQUIRE(Interpret("game 'common'", dir.Path()).manifest.IsCompatibleWithGta5());
    REQUIRE(Interpret("games { 'gta5', 'rdr3' }", dir.Path()).manifest.IsCompatibleWithGta5());
    REQUIRE(Interpret("game 'GTA5'", dir.Path()).manifest.IsCompatibleWithGta5());
    // Nothing said means no objection.
    REQUIRE(Interpret("fx_version 'cerulean'", dir.Path()).manifest.IsCompatibleWithGta5());

    REQUIRE_FALSE(Interpret("games { 'rdr3' }", dir.Path()).manifest.IsCompatibleWithGta5());
    REQUIRE(Interpret("games { 'rdr3' }", dir.Path()).manifest.DescribeGames() == "rdr3");
}

TEST_CASE("ResourceManifest: data_file patterns resolve against the resource root", "[manifest]")
{
    const TempDir dir;
    dir.WriteFile("stream/a.ytyp", "x");
    dir.WriteFile("stream/b.ytyp", "x");

    const Interpreted result =
        Interpret("data_file 'DLC_ITYP_REQUEST' 'stream/*.ytyp'", dir.Path());

    REQUIRE(result.manifest.dataFiles.size() == 1);
    REQUIRE(result.manifest.dataFiles.front().type == "DLC_ITYP_REQUEST");
    REQUIRE(result.manifest.dataFiles.front().resolved ==
            std::vector<std::string>{"stream/a.ytyp", "stream/b.ytyp"});
}

TEST_CASE("ResourceManifest: the data_file type is upper-cased", "[manifest]")
{
    const TempDir dir;
    dir.WriteFile("x.ytyp", "x");

    REQUIRE(Interpret("data_file 'dlc_ityp_request' 'x.ytyp'", dir.Path())
                .manifest.dataFiles.front()
                .type == "DLC_ITYP_REQUEST");
}

TEST_CASE("ResourceManifest: a literal pattern matching nothing is kept as written", "[manifest]")
{
    const TempDir dir;

    // Some data-file types take a name rather than a path, so the literal survives.
    const Interpreted result = Interpret("data_file 'DLC_ITYP_REQUEST' 'props'", dir.Path());

    REQUIRE(result.manifest.dataFiles.size() == 1);
    REQUIRE(result.manifest.dataFiles.front().resolved == std::vector<std::string>{"props"});
}

TEST_CASE("ResourceManifest: a wildcard pattern matching nothing warns and is dropped",
          "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret("data_file 'X' 'stream/*.ytyp'", dir.Path());

    REQUIRE(result.manifest.dataFiles.empty());
    REQUIRE(result.Mentions("matched no files"));
}

TEST_CASE("ResourceManifest: a count mismatch drops every data file", "[manifest]")
{
    const TempDir dir;
    dir.WriteFile("a.ytyp", "x");

    // The second data_file has no path, so the two lists no longer line up.
    const Interpreted result = Interpret(R"(
data_file 'DLC_ITYP_REQUEST' 'a.ytyp'
data_file 'HANDLING_FILE'
)",
                                         dir.Path());

    REQUIRE(result.manifest.dataFiles.empty());
    REQUIRE(result.HasError());
    REQUIRE(result.Mentions("count mismatch"));
}

TEST_CASE("ResourceManifest: a data_file in another resource is kept for the plan to resolve",
          "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret("data_file 'X' '@other/data/*.meta'", dir.Path());

    REQUIRE(result.manifest.dataFiles.size() == 1);
    const spl::manifest::DataFileEntry& entry = result.manifest.dataFiles.front();
    REQUIRE(entry.otherResource == "other");
    REQUIRE(entry.pattern == "data/*.meta");
    REQUIRE(entry.resolved.empty());
}

TEST_CASE("ResourceManifest: an @ path without a resource and a file is refused", "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret("data_file 'X' '@other'", dir.Path());

    REQUIRE(result.manifest.dataFiles.empty());
    REQUIRE(result.Mentions("@resource/path"));
}

TEST_CASE("ResourceManifest: script entries are counted, not loaded", "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret(R"(
client_scripts { 'a.lua', 'b.lua' }
server_script 'c.lua'
ui_page 'index.html'
)",
                                         dir.Path());

    REQUIRE(result.manifest.ignoredScriptEntries == 4);
}

TEST_CASE("ResourceManifest: level metas are collected, level replacement is unsupported",
          "[manifest]")
{
    const TempDir dir;
    const Interpreted result = Interpret(R"(
init_meta 'data/init.meta'
before_level_meta 'data/before.meta'
after_level_meta { 'data/after_a.meta', 'data/after_b.meta' }
replace_level_meta 'levels/x.meta'
)",
                                         dir.Path());

    REQUIRE(result.manifest.initMetas == std::vector<std::string>{"data/init.meta"});
    REQUIRE(result.manifest.beforeLevelMetas == std::vector<std::string>{"data/before.meta"});
    REQUIRE(result.manifest.afterLevelMetas ==
            std::vector<std::string>{"data/after_a.meta", "data/after_b.meta"});
    REQUIRE(result.manifest.unsupportedKeys == std::vector<std::string>{"replace_level_meta"});
    REQUIRE(result.Mentions("not supported"));
}

TEST_CASE("ResourceManifest: IsScriptKey covers both singular and plural forms", "[manifest]")
{
    REQUIRE(spl::manifest::IsScriptKey("client_script"));
    REQUIRE(spl::manifest::IsScriptKey("client_scripts"));
    REQUIRE(spl::manifest::IsScriptKey("ui_page"));
    REQUIRE_FALSE(spl::manifest::IsScriptKey("this_is_a_map"));
    REQUIRE_FALSE(spl::manifest::IsScriptKey("data_file"));
}
