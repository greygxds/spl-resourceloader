#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "mods/ModsScanner.h"
#include "tests/TempTree.h"
#include "tests/rpf/RpfBuilder.h"

using spl::mods::DiscoveredMod;
using spl::mods::ModsScanner;
using spl::tests::RpfBuilder;
using spl::tests::TempDir;

namespace
{
constexpr std::string_view kAssembly =
    R"xml(<package id="{62AB8F34-BE20-46D5-9F0F-84729F087E5E}" target="Five">
  <content><archive path="common.rpf"><add source="a.ytd">a.ytd</add></archive></content>
</package>)xml";

void AddMod(const TempDir& dir, std::string_view file, const std::string& archive)
{
    dir.WriteFile(std::string{"mods/"} + std::string{file}, archive);
}

[[nodiscard]] std::string ModArchive(std::string_view assembly)
{
    RpfBuilder builder;
    builder.AddFile("assembly.xml", assembly);
    builder.AddFile("content/a.ytd", "bytes");
    return builder.Build();
}
} // namespace

TEST_CASE("ModsScanner: finds and parses mods in file order", "[mods]")
{
    TempDir dir;
    AddMod(dir, "zebra.rpf", ModArchive(kAssembly));
    AddMod(dir, "Alpha.RPF", ModArchive(kAssembly)); // wrong case: not a mod (FiveM :154)
    AddMod(dir, "apple.rpf", ModArchive(kAssembly));
    AddMod(dir, "notes.txt", "not an archive");

    const ModsScanner::Result scanned = ModsScanner::Scan(dir.Path() / "mods");

    REQUIRE(scanned.mods.size() == 2);
    CHECK(scanned.mods[0].name == "apple");
    CHECK(scanned.mods[1].name == "zebra");
    CHECK(scanned.mods[0].package.entries.size() == 1);
    CHECK(scanned.warnings.empty());
    CHECK(scanned.ignoredArchives == std::vector<std::string>{"Alpha.RPF"});
}

TEST_CASE("ModsScanner: reads a deflated assembly with a BOM", "[mods]")
{
    // Real OpenIV shape (KWTR packs): assembly.xml is deflated and BOM-prefixed.
    TempDir dir;
    RpfBuilder builder;
    builder.AddCompressedFile("assembly.xml", "\xEF\xBB\xBF" + std::string{kAssembly});
    builder.AddFile("content/a.ytd", "bytes");
    AddMod(dir, "kwtr.rpf", builder.Build());

    const ModsScanner::Result scanned = ModsScanner::Scan(dir.Path() / "mods");

    REQUIRE(scanned.mods.size() == 1);
    CHECK(scanned.mods[0].name == "kwtr");
    CHECK(scanned.mods[0].package.entries.size() == 1);
    CHECK(scanned.warnings.empty());
}

TEST_CASE("ModsScanner: skips broken mods with warnings", "[mods]")
{
    TempDir dir;
    AddMod(dir, "corrupt.rpf", "not an archive at all");
    AddMod(dir, "noassembly.rpf", RpfBuilder{}.Build());
    AddMod(dir, "badxml.rpf", ModArchive("<package target=\"Five\"><content>"));
    AddMod(dir, "empty.rpf", ModArchive("<package target=\"Five\"></package>"));
    AddMod(dir, "good.rpf", ModArchive(kAssembly));

    const ModsScanner::Result scanned = ModsScanner::Scan(dir.Path() / "mods");

    REQUIRE(scanned.mods.size() == 1);
    CHECK(scanned.mods[0].name == "good");
    CHECK(scanned.warnings.size() == 3); // corrupt, no assembly, broken xml; empty is silent
}

TEST_CASE("ModsScanner: names outside the system code page are read as UTF-8", "[mods]")
{
    // Cyrillic and CJK together fit no single ANSI code page, so path::string() would throw.
    const std::u8string stem = u8"машина_车";
    const std::u8string brokenStem = u8"сломан_坏";
    TempDir dir;
    dir.AddDirectory("mods");
    const auto write = [&dir](const std::u8string& name, const std::string& contents)
    {
        std::ofstream stream{dir.Path() / "mods" / std::filesystem::path{name + u8".rpf"},
                             std::ios::binary};
        stream << contents;
    };
    write(stem, ModArchive(kAssembly));
    write(brokenStem, "not an archive at all");

    const ModsScanner::Result scanned = ModsScanner::Scan(dir.Path() / "mods");

    const auto utf8 = [](const std::u8string& text)
    { return std::string{reinterpret_cast<const char*>(text.data()), text.size()}; };
    REQUIRE(scanned.mods.size() == 1);
    CHECK(scanned.mods[0].name == utf8(stem));
    REQUIRE(scanned.warnings.size() == 1);
    CHECK(scanned.warnings[0].find(utf8(brokenStem)) != std::string::npos);
}

TEST_CASE("ModsScanner: a missing folder is not an error", "[mods]")
{
    const ModsScanner::Result scanned = ModsScanner::Scan("does-not-exist");

    CHECK(scanned.mods.empty());
    CHECK(scanned.warnings.empty());
}

TEST_CASE("ModsScanner: Select drops disabled mods and puts priority first", "[mods]")
{
    std::vector<DiscoveredMod> mods{
        {.name = "alpha"}, {.name = "Bravo"}, {.name = "charlie"}, {.name = "delta"}};
    spl::config::ModsSettings settings;
    settings.disabled = {"BRAVO"};
    settings.priority = {"delta", "missing", "charlie"};

    const ModsScanner::Selection selection = ModsScanner::Select(mods, settings);

    std::vector<std::string> names;
    for (const DiscoveredMod& mod : mods)
    {
        names.push_back(mod.name);
    }
    CHECK(names == std::vector<std::string>{"delta", "charlie", "alpha"});
    CHECK(selection.disabled == std::vector<std::string>{"Bravo"});
    CHECK(selection.missingPriority == std::vector<std::string>{"missing"});
}

TEST_CASE("ModsScanner: Select with default settings changes nothing", "[mods]")
{
    std::vector<DiscoveredMod> mods{{.name = "alpha"}, {.name = "bravo"}};

    const ModsScanner::Selection selection = ModsScanner::Select(mods, {});

    REQUIRE(mods.size() == 2);
    CHECK(mods[0].name == "alpha");
    CHECK(mods[1].name == "bravo");
    CHECK(selection.disabled.empty());
    CHECK(selection.missingPriority.empty());
}
