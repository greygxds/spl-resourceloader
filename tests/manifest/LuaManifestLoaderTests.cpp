#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "manifest/LuaManifestLoader.h"
#include "manifest/ManifestParser.h"

using spl::manifest::LoadManifestWithLua;
using spl::manifest::LuaSandboxLimits;
using spl::manifest::ManifestDiagnostic;
using spl::manifest::ManifestEntry;
using spl::manifest::ManifestParser;
using spl::manifest::ParseResult;

namespace
{
using Pair = std::pair<std::string, std::string>;
using PairList = std::vector<Pair>;

PairList EntryPairs(const ParseResult& result)
{
    PairList pairs;
    for (const ManifestEntry& entry : result.document.All())
    {
        pairs.emplace_back(entry.key, entry.value);
    }
    return pairs;
}

ParseResult Run(std::string_view source)
{
    return LoadManifestWithLua(source, "fxmanifest.lua");
}

bool Mentions(const ParseResult& result, std::string_view text)
{
    return std::ranges::any_of(result.diagnostics, [&](const ManifestDiagnostic& diagnostic)
                               { return diagnostic.message.find(text) != std::string::npos; });
}
} // namespace

// The metatable semantics, run through the real interpreter this time. These mirror the
// ManifestParser cases, so the two backends are held to the same behavior.

TEST_CASE("LuaManifestLoader: a string argument keeps the key unchanged", "[manifest][lua]")
{
    REQUIRE(EntryPairs(Run("fx_version 'cerulean'")) == PairList{{"fx_version", "cerulean"}});
    REQUIRE(EntryPairs(Run("client_scripts 'a.lua'")) == PairList{{"client_scripts", "a.lua"}});
}

TEST_CASE("LuaManifestLoader: a table argument strips one trailing s", "[manifest][lua]")
{
    REQUIRE(EntryPairs(Run("games { 'gta5', 'rdr3' }")) ==
            PairList{{"game", "gta5"}, {"game", "rdr3"}});
}

TEST_CASE("LuaManifestLoader: a second argument becomes a JSON _extra", "[manifest][lua]")
{
    REQUIRE(
        EntryPairs(Run("data_file 'DLC_ITYP_REQUEST' 'stream/props.ytyp'")) ==
        PairList{{"data_file", "DLC_ITYP_REQUEST"}, {"data_file_extra", "\"stream/props.ytyp\""}});
    REQUIRE(EntryPairs(Run("data_file('X')('y.ytyp')")) ==
            PairList{{"data_file", "X"}, {"data_file_extra", "\"y.ytyp\""}});
    REQUIRE(EntryPairs(Run("my_data 'a' { 'b' }")) ==
            PairList{{"my_data", "a"}, {"my_data_extra", "[\"b\"]"}});
}

TEST_CASE("LuaManifestLoader: _extra uses the de-pluralized key of a table argument",
          "[manifest][lua]")
{
    REQUIRE(EntryPairs(Run("files { 'a' } { 'b' }")) ==
            PairList{{"file", "a"}, {"file_extra", "[\"b\"]"}});
}

TEST_CASE("LuaManifestLoader: is_cfxv2 is never taken from the manifest", "[manifest][lua]")
{
    REQUIRE_FALSE(Run("is_cfxv2 'true'").document.Has("is_cfxv2"));
}

TEST_CASE("LuaManifestLoader: entries carry the line they came from", "[manifest][lua]")
{
    const ParseResult result = Run("fx_version 'c'\n\ngame 'gta5'");

    REQUIRE(result.document.GetEntries("game").front()->line == 3);
}

// What the interpreter buys us over reading the file as text.

TEST_CASE("LuaManifestLoader: a computed file list works", "[manifest][lua]")
{
    const ParseResult result = Run(R"(
local list = {}
for i = 1, 3 do
    list[#list + 1] = ('stream/part%d.ytd'):format(i)
end
files(list)
)");

    REQUIRE_FALSE(result.fatal);
    REQUIRE(EntryPairs(result) == PairList{{"file", "stream/part1.ytd"},
                                           {"file", "stream/part2.ytd"},
                                           {"file", "stream/part3.ytd"}});
}

TEST_CASE("LuaManifestLoader: a conditional block is evaluated, not skipped", "[manifest][lua]")
{
    const ParseResult result = Run(R"(
local isMap = true
if isMap then
    this_is_a_map 'yes'
else
    this_is_a_map 'no'
end
)");

    REQUIRE(result.document.GetEntries("this_is_a_map").front()->value == "yes");
}

TEST_CASE("LuaManifestLoader: string concatenation works", "[manifest][lua]")
{
    REQUIRE(EntryPairs(Run("local v = '1.0'\nversion('mod-' .. v)")) ==
            PairList{{"version", "mod-1.0"}});
}

// The sandbox. A manifest is code from a folder the user downloaded.

TEST_CASE("LuaManifestLoader: no host library is reachable", "[manifest][lua]")
{
    // Undefined globals resolve to a metadata function, so these names are not nil; what
    // matters is that none of them is the real library table.
    const auto library = GENERATE(as<std::string>{}, "io", "os", "package", "debug", "coroutine");

    const ParseResult result = Run("kind(type(" + library + "))");

    INFO("library: " << library);
    REQUIRE(result.document.GetEntries("kind").front()->value == "function");
}

TEST_CASE("LuaManifestLoader: reaching into a host library fails", "[manifest][lua]")
{
    // io.open on a metadata stub is an attempt to index a function value, so it raises.
    const auto expression =
        GENERATE(as<std::string>{}, "io.open('x', 'w')", "os.execute('dir')", "os.remove('x')",
                 "package.loadlib('x', 'y')", "debug.getinfo(1)");

    const ParseResult result =
        Run("worked(tostring(pcall(function() return " + expression + " end)))");

    INFO("expression: " << expression);
    REQUIRE(result.document.GetEntries("worked").front()->value == "false");
}

TEST_CASE("LuaManifestLoader: the real code loaders are gone", "[manifest][lua]")
{
    // luaopen_base defines load, dofile and loadfile, so they have to be removed by name:
    // whatever is left must not be able to compile a string into a function.
    const ParseResult result =
        Run("compiled(tostring(pcall(function() return load('return 1')() + 1 end)))");

    REQUIRE(result.document.GetEntries("compiled").front()->value == "false");
}

TEST_CASE("LuaManifestLoader: an endless loop is stopped", "[manifest][lua]")
{
    LuaSandboxLimits limits;
    limits.instructionBudget = 10'000; // keep the test quick

    const ParseResult result =
        LoadManifestWithLua("game 'gta5'\nwhile true do end", "fxmanifest.lua", limits);

    REQUIRE(result.fatal);
    REQUIRE(Mentions(result, "instruction budget"));
    // Whatever ran before the loop is still there.
    REQUIRE(result.document.Has("game"));
}

TEST_CASE("LuaManifestLoader: an endless loop inside pcall is stopped too", "[manifest][lua]")
{
    LuaSandboxLimits limits;
    limits.instructionBudget = 10'000;

    const ParseResult result = LoadManifestWithLua(
        "game 'gta5'\nwhile true do pcall(function() while true do end end) end", "fxmanifest.lua",
        limits);

    REQUIRE(result.fatal);
    REQUIRE(Mentions(result, "instruction budget"));
}

TEST_CASE("LuaManifestLoader: runaway allocation is stopped", "[manifest][lua]")
{
    LuaSandboxLimits limits;
    limits.memoryBudgetBytes = 1u * 1024 * 1024;

    const ParseResult result = LoadManifestWithLua(R"(
local t = {}
for i = 1, 1000000 do t[i] = string.rep('x', 1024) end
files(t)
)",
                                                   "fxmanifest.lua", limits);

    REQUIRE(result.fatal);
}

TEST_CASE("LuaManifestLoader: precompiled bytecode is refused", "[manifest][lua]")
{
    // A chunk starting with the Lua signature byte is binary, and binary chunks are not
    // verified, so the loader must never accept one.
    const ParseResult result = Run(std::string{"\x1b"} + "Lua rest of a binary chunk");

    REQUIRE(result.fatal);
}

TEST_CASE("LuaManifestLoader: a syntax error is reported with its line", "[manifest][lua]")
{
    const ParseResult result = Run("game 'gta5'\nthis is not lua ===");

    REQUIRE(result.fatal);
    REQUIRE(Mentions(result, "fxmanifest.lua:"));
}

TEST_CASE("LuaManifestLoader: an error that is not a string is still reported", "[manifest][lua]")
{
    const auto chunk =
        GENERATE(as<std::string>{}, "game 'gta5'\nerror({})", "game 'gta5'\nerror()");

    const ParseResult result = Run(chunk);

    INFO("chunk: " << chunk);
    REQUIRE(result.fatal);
    REQUIRE(Mentions(result, "as an error"));
    REQUIRE(result.document.Has("game"));
}

TEST_CASE("LuaManifestLoader: an error mid-chunk keeps what ran before it", "[manifest][lua]")
{
    const ParseResult result = Run("game 'gta5'\nerror('boom')");

    REQUIRE(result.fatal);
    REQUIRE(result.document.Has("game"));
}

// ManifestParser::Parse chooses between the two backends.

TEST_CASE("ManifestParser: Parse uses Lua and so evaluates logic", "[manifest][lua]")
{
    const ParseResult result = ManifestParser::Parse(R"(
for i = 1, 2 do
    file('generated' .. i .. '.meta')
end
)",
                                                     "fxmanifest.lua");

    REQUIRE(EntryPairs(result) ==
            PairList{{"file", "generated1.meta"}, {"file", "generated2.meta"}});
}

TEST_CASE("ManifestParser: Parse falls back to text when the chunk will not run", "[manifest][lua]")
{
    // Valid declarative lines around something Lua cannot compile: the text parser recovers
    // and still finds both entries, where the interpreter alone would have found none.
    const ParseResult result = ManifestParser::Parse(R"(
game 'gta5'
this is not lua ===
this_is_a_map 'yes'
)",
                                                     "fxmanifest.lua");

    REQUIRE(result.document.Has("game"));
    REQUIRE(result.document.Has("this_is_a_map"));
    REQUIRE(Mentions(result, "read as plain text"));
}

TEST_CASE("ManifestParser: the fallback does not duplicate entries", "[manifest][lua]")
{
    const ParseResult result = ManifestParser::Parse("game 'gta5'\nbroken ===", "fxmanifest.lua");

    REQUIRE(result.document.GetEntries("game").size() == 1);
}
