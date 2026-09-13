#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "manifest/ManifestParser.h"

using spl::manifest::ManifestDiagnostic;
using spl::manifest::ManifestEntry;
using spl::manifest::ManifestParser;
using spl::manifest::ParseResult;

namespace
{
/// These cases are about the text parser specifically: how it recovers from Lua it does not
/// interpret. ManifestParser::Parse runs the interpreter first, which is covered separately in
/// LuaManifestLoaderTests.
ParseResult Parse(std::string_view source)
{
    return ManifestParser::ParseText(source, "fxmanifest.lua");
}

/// Every (key, value) pair in order, which is what the metatable in resource_init.lua
/// would have handed to AddMetaData.
std::vector<std::pair<std::string, std::string>> EntryPairs(const ParseResult& result)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const ManifestEntry& entry : result.document.All())
    {
        pairs.emplace_back(entry.key, entry.value);
    }
    return pairs;
}

bool HasWarning(const ParseResult& result)
{
    return std::ranges::any_of(
        result.diagnostics, [](const ManifestDiagnostic& diagnostic)
        { return diagnostic.severity == ManifestDiagnostic::Severity::Warning; });
}

using Pair = std::pair<std::string, std::string>;
using Pairs = std::vector<Pair>;
} // namespace

// The argument forms a manifest key accepts, one case per form.

TEST_CASE("ManifestParser: a string argument keeps the key unchanged", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("fx_version 'cerulean'")) == Pairs{{"fx_version", "cerulean"}});
    REQUIRE(EntryPairs(Parse("game \"gta5\"")) == Pairs{{"game", "gta5"}});
    REQUIRE(EntryPairs(Parse("client_scripts 'a.lua'")) == Pairs{{"client_scripts", "a.lua"}});
    REQUIRE(EntryPairs(Parse("this_is_a_map 'yes'")) == Pairs{{"this_is_a_map", "yes"}});
}

TEST_CASE("ManifestParser: a table argument strips one trailing s", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("games { 'gta5', 'rdr3' }")) ==
            Pairs{{"game", "gta5"}, {"game", "rdr3"}});
    REQUIRE(EntryPairs(Parse("files { 'a.meta', 'b.meta' }")) ==
            Pairs{{"file", "a.meta"}, {"file", "b.meta"}});
}

TEST_CASE("ManifestParser: a key without a trailing s is unchanged by a table", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("game { 'gta5' }")) == Pairs{{"game", "gta5"}});
}

TEST_CASE("ManifestParser: a second argument becomes a JSON-encoded _extra", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("data_file 'DLC_ITYP_REQUEST' 'stream/props.ytyp'")) ==
            Pairs{{"data_file", "DLC_ITYP_REQUEST"}, {"data_file_extra", "\"stream/props.ytyp\""}});
}

TEST_CASE("ManifestParser: the parenthesized call form is equivalent", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("data_file('DLC_ITYP_REQUEST')('x.ytyp')")) ==
            Pairs{{"data_file", "DLC_ITYP_REQUEST"}, {"data_file_extra", "\"x.ytyp\""}});
}

TEST_CASE("ManifestParser: a table second argument becomes a JSON array", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("my_data 'a' { 'b' }")) ==
            Pairs{{"my_data", "a"}, {"my_data_extra", "[\"b\"]"}});
}

TEST_CASE("ManifestParser: _extra uses the de-pluralized key of a table first argument",
          "[manifest]")
{
    // newK in resource_init.lua is de-pluralized only when the FIRST value was a table.
    REQUIRE(EntryPairs(Parse("files { 'a' } { 'b' }")) ==
            Pairs{{"file", "a"}, {"file_extra", "[\"b\"]"}});
}

TEST_CASE("ManifestParser: the decoded form accompanies every entry", "[manifest]")
{
    const ParseResult result = Parse("data_file 'X' 'stream/a.ytyp'");
    const auto extra = result.document.GetEntries("data_file_extra");

    REQUIRE(extra.size() == 1);
    REQUIRE(extra.front()->value == "\"stream/a.ytyp\"");
    REQUIRE(extra.front()->decoded == std::vector<std::string>{"stream/a.ytyp"});
}

TEST_CASE("ManifestParser: is_cfxv2 is never taken from the manifest", "[manifest]")
{
    const ParseResult result = Parse("is_cfxv2 'true'\nfx_version 'cerulean'");

    REQUIRE_FALSE(result.document.Has("is_cfxv2"));
    REQUIRE(result.document.Has("fx_version"));
}

TEST_CASE("ManifestParser: JSON encoding escapes what it must", "[manifest]")
{
    REQUIRE(spl::manifest::EncodeJsonString("a\"b\\c") == "\"a\\\"b\\\\c\"");
    REQUIRE(spl::manifest::EncodeJsonString("line\nbreak") == "\"line\\nbreak\"");
    REQUIRE(spl::manifest::EncodeJsonArray({"a", "b"}) == "[\"a\",\"b\"]");
    REQUIRE(spl::manifest::EncodeJsonArray({}) == "[]");
}

TEST_CASE("ManifestParser: a third argument warns and is ignored", "[manifest]")
{
    const ParseResult result = Parse("key 'a' 'b' 'c'");

    REQUIRE(EntryPairs(result) == Pairs{{"key", "a"}, {"key_extra", "\"b\""}});
    REQUIRE(HasWarning(result));
}

TEST_CASE("ManifestParser: statements may be separated by semicolons and newlines", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("fx_version 'c';\ngame 'gta5';")) ==
            Pairs{{"fx_version", "c"}, {"game", "gta5"}});
}

TEST_CASE("ManifestParser: named table fields are skipped", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("files { 'a', extra = 'no', 'b' }")) ==
            Pairs{{"file", "a"}, {"file", "b"}});
}

TEST_CASE("ManifestParser: a trailing separator in a table is allowed", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("files { 'a', 'b', }")) == Pairs{{"file", "a"}, {"file", "b"}});
    REQUIRE(EntryPairs(Parse("files { 'a'; 'b'; }")) == Pairs{{"file", "a"}, {"file", "b"}});
}

TEST_CASE("ManifestParser: an empty table produces nothing", "[manifest]")
{
    REQUIRE(EntryPairs(Parse("files {}")).empty());
}

// Recovery from Lua we do not interpret.

TEST_CASE("ManifestParser: an assignment is skipped and parsing continues", "[manifest]")
{
    const ParseResult result = Parse("local x = 'a'\nthis_is_a_map 'yes'");

    REQUIRE(result.document.Has("this_is_a_map"));
    REQUIRE(HasWarning(result));
}

TEST_CASE("ManifestParser: an if block is skipped as a unit", "[manifest]")
{
    const ParseResult result = Parse(R"(
if GetConvar('x') == 'y' then
    client_script 'a.lua'
end
this_is_a_map 'yes'
)");

    REQUIRE(result.document.Has("this_is_a_map"));
    // The statement inside the skipped block must not have been collected.
    REQUIRE_FALSE(result.document.Has("client_script"));
}

TEST_CASE("ManifestParser: a function block is skipped as a unit", "[manifest]")
{
    const ParseResult result = Parse(R"(
function helper()
    return 1
end
game 'gta5'
)");

    REQUIRE(EntryPairs(result) == Pairs{{"game", "gta5"}});
}

TEST_CASE("ManifestParser: a for block is skipped as a unit", "[manifest]")
{
    const ParseResult result = Parse(R"(
for i = 1, 10 do
    file 'x'
end
game 'gta5'
)");

    REQUIRE(EntryPairs(result) == Pairs{{"game", "gta5"}});
}

TEST_CASE("ManifestParser: a fatal lexer error stops parsing but keeps earlier entries",
          "[manifest]")
{
    const ParseResult result = Parse("game 'gta5'\nname 'unterminated");

    REQUIRE(result.fatal);
    REQUIRE(result.document.Has("game"));
    REQUIRE(std::ranges::any_of(
        result.diagnostics, [](const ManifestDiagnostic& diagnostic)
        { return diagnostic.severity == ManifestDiagnostic::Severity::Error; }));
}

TEST_CASE("ManifestParser: diagnostics carry a line number", "[manifest]")
{
    const ParseResult result = Parse("game 'gta5'\n\nlocal x = 1\n");

    REQUIRE_FALSE(result.diagnostics.empty());
    REQUIRE(result.diagnostics.front().line == 3);
}

TEST_CASE("ManifestParser: a realistic map manifest reads correctly", "[manifest]")
{
    const ParseResult result = Parse(R"(
fx_version 'cerulean'
game 'gta5'

author 'Someone'
description 'A test map'
version '1.0.0'

this_is_a_map 'yes'

data_file 'DLC_ITYP_REQUEST' 'stream/props.ytyp'
data_file 'HANDLING_FILE' 'data/handling.meta'

files {
    'data/handling.meta',
}

client_scripts {
    'client/main.lua',
}
)");

    REQUIRE_FALSE(result.fatal);
    REQUIRE(result.document.Has("this_is_a_map"));
    REQUIRE(result.document.GetEntries("data_file").size() == 2);
    REQUIRE(result.document.GetEntries("data_file_extra").size() == 2);
    REQUIRE(result.document.GetEntries("file").size() == 1);
    REQUIRE(result.document.GetEntries("client_script").size() == 1);
    REQUIRE(result.document.GetEntries("game").front()->value == "gta5");
}
