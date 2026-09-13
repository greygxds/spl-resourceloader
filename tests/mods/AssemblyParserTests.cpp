#include <string_view>

#include <catch_amalgamated.hpp>

#include "mods/AssemblyParser.h"

using spl::mods::AssemblyParser;
using spl::mods::AssemblyResult;
using spl::mods::ContentResult;
using spl::mods::Setup2Result;

namespace
{
constexpr std::string_view kPackage = R"xml(<?xml version="1.0" encoding="utf-8"?>
<!-- a typical OpenIV export -->
<package id="{62AB8F34-BE20-46D5-9F0F-84729F087E5E}" target="Five">
  <metadata>
    <name>Example</name>
    <version><major>1</major><minor>2</minor></version>
    <author><displayName>bob</displayName></author>
    <description>does things</description>
  </metadata>
  <content>
    <archive path="update\update.rpf">
      <archive path="x64\textures.rpf">
        <add source="content\car.ytd">textures\car.ytd</add>
      </archive>
      <add source="content\game.dat">common\data\game.dat</add>
    </archive>
    <archive path="common.rpf">
      <add source="content\map.ymap">map.ymap</add>
    </archive>
  </content>
</package>
)xml";

[[nodiscard]] bool HasSeverity(const std::vector<spl::mods::AssemblyDiagnostic>& diagnostics,
                               spl::mods::AssemblyDiagnostic::Severity severity)
{
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.severity == severity)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("AssemblyParser: parses a representative package", "[mods]")
{
    const AssemblyResult parsed = AssemblyParser::ParseAssembly(kPackage);

    REQUIRE(!parsed.fatal);
    CHECK(parsed.diagnostics.empty());
    CHECK(parsed.package.guid == "62AB8F34-BE20-46D5-9F0F-84729F087E5E");
    CHECK(parsed.package.metadata.name == "Example");
    CHECK(parsed.package.metadata.version == "1.2");
    CHECK(parsed.package.metadata.authorName == "bob");
    CHECK(parsed.package.metadata.description == "does things");

    REQUIRE(parsed.package.entries.size() == 3);
    CHECK(parsed.package.entries[0].archiveRoots ==
          std::vector<std::string>{"update\\update.rpf", "x64\\textures.rpf"});
    CHECK(parsed.package.entries[0].sourceFile == "content\\car.ytd");
    CHECK(parsed.package.entries[0].targetFile == "textures\\car.ytd");
    CHECK(parsed.package.entries[1].archiveRoots == std::vector<std::string>{"update\\update.rpf"});
    CHECK(parsed.package.entries[2].archiveRoots == std::vector<std::string>{"common.rpf"});
    CHECK(parsed.package.entries[2].targetFile == "map.ymap");
}

TEST_CASE("AssemblyParser: rejects non-package documents", "[mods]")
{
    const AssemblyResult wrongRoot = AssemblyParser::ParseAssembly("<mod></mod>");
    CHECK(wrongRoot.fatal);
    CHECK(HasSeverity(wrongRoot.diagnostics, spl::mods::AssemblyDiagnostic::Severity::Error));

    const AssemblyResult wrongTarget =
        AssemblyParser::ParseAssembly("<package target=\"RedM\"></package>");
    CHECK(wrongTarget.fatal);

    const AssemblyResult empty = AssemblyParser::ParseAssembly("");
    CHECK(empty.fatal);

    const AssemblyResult mismatched =
        AssemblyParser::ParseAssembly("<package target=\"Five\"><content></package>");
    CHECK(mismatched.fatal);
}

TEST_CASE("AssemblyParser: zeroes a malformed guid without failing", "[mods]")
{
    const AssemblyResult parsed =
        AssemblyParser::ParseAssembly("<package id=\"not-a-guid\" target=\"Five\"></package>");

    REQUIRE(!parsed.fatal);
    CHECK(parsed.package.guid == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("AssemblyParser: notes unknown elements and decodes entities", "[mods]")
{
    const AssemblyResult parsed = AssemblyParser::ParseAssembly(
        "<package target=\"Five\"><content><archive path=\"x.rpf\">"
        "<add source='a &amp; b'>x &lt;y&gt;</add><frob/></archive></content></package>");

    REQUIRE(!parsed.fatal);
    REQUIRE(parsed.package.entries.size() == 1);
    CHECK(parsed.package.entries[0].sourceFile == "a & b");
    CHECK(parsed.package.entries[0].targetFile == "x <y>");
    CHECK(HasSeverity(parsed.diagnostics, spl::mods::AssemblyDiagnostic::Severity::Info));
}

TEST_CASE("AssemblyParser: skips a UTF-8 BOM and rejects UTF-16", "[mods]")
{
    // Real assemblies (KWTR packs) start with a BOM, which tinyxml2 skips.
    const AssemblyResult parsed =
        AssemblyParser::ParseAssembly("\xEF\xBB\xBF<package target=\"Five\"></package>");

    REQUIRE(!parsed.fatal);
    CHECK(parsed.diagnostics.empty());

    const std::string utf16 = "\xFF\xFE<\x00p\x00";
    const AssemblyResult wide = AssemblyParser::ParseAssembly(utf16);
    CHECK(wide.fatal);
}

TEST_CASE("AssemblyParser: parses setup2 descriptors", "[mods]")
{
    const Setup2Result parsed =
        AssemblyParser::ParseSetup2("<setup2><deviceName>exDlc</deviceName><order value=\"7\"/>"
                                    "<requiredVersion>2944-3095</requiredVersion></setup2>");

    REQUIRE(!parsed.fatal);
    CHECK(parsed.descriptor.deviceName == "exDlc");
    CHECK(parsed.descriptor.order == 7);
    CHECK(parsed.descriptor.requiredVersion == "2944-3095");

    const Setup2Result minimal =
        AssemblyParser::ParseSetup2("<setup2><deviceName>exDlc</deviceName></setup2>");
    REQUIRE(!minimal.fatal);
    CHECK(minimal.descriptor.order == 0);
    CHECK(minimal.descriptor.requiredVersion.empty());
}

TEST_CASE("AssemblyParser: parses content.xml items verbatim", "[mods]")
{
    const ContentResult parsed = AssemblyParser::ParseContent(
        "<content><dataFiles>"
        "<Item><filename>%PLATFORM%/dlc.rpf</filename><fileType>RPF_FILE</fileType></Item>"
        "<Item><filename>handling.meta</filename><fileType>HANDLING_FILE</fileType></Item>"
        "</dataFiles></content>");

    REQUIRE(!parsed.fatal);
    REQUIRE(parsed.items.size() == 2);
    CHECK(parsed.items[0].filename == "%PLATFORM%/dlc.rpf"); // M5 substitutes this
    CHECK(parsed.items[0].fileType == "RPF_FILE");
    CHECK(parsed.items[1].filename == "handling.meta");
}
