#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "core/Result.h"
#include "manifest/ManifestParser.h"
#include "mods/ModLayout.h"
#include "mods/ModPackage.h"
#include "mods/ModsScanner.h"
#include "tests/TempTree.h"
#include "tests/rpf/RpfBuilder.h"

using spl::manifest::ManifestEntry;
using spl::manifest::ManifestParser;
using spl::manifest::ParseResult;
using spl::mods::DiscoveredMod;
using spl::mods::ModEntry;
using spl::mods::ModLayout;
using spl::mods::ModPackage;
using spl::tests::RpfBuilder;
using spl::tests::TempDir;

namespace
{
[[nodiscard]] ModEntry Add(std::vector<std::string> roots, std::string source, std::string target)
{
    return ModEntry{.archiveRoots = std::move(roots),
                    .sourceFile = std::move(source),
                    .targetFile = std::move(target)};
}

/// Writes an .rpf holding exactly the sources the package names, and describes the mod.
[[nodiscard]] DiscoveredMod WriteMod(const TempDir& dir, std::string_view name,
                                     std::vector<ModEntry> entries)
{
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/car.yft", "model");
    builder.AddFile("content/handling.meta", "handling");
    builder.AddFile("content/custom.meta", "custom");
    builder.AddFile("content/one.ytd", "one");
    builder.AddFile("content/two.ytd", "two");

    RpfBuilder nested;
    nested.AddFile("inner.ytd", "nested");
    nested.AddFile("sub/deep.ytd", "deep");
    builder.AddFile("content/extra.rpf", nested.Build());

    const std::string file = std::string{name} + ".rpf";
    dir.WriteFile(file, builder.Build());

    ModPackage package;
    package.entries = std::move(entries);
    return DiscoveredMod{.name = std::string{name},
                         .absolutePath = dir.Path() / file,
                         .package = std::move(package)};
}

/// What the layout serves at a mod-relative path, read the way the loader reads it.
[[nodiscard]] std::string Read(const ModLayout::Result& laid, std::string_view relative)
{
    const spl::Result<std::string> contents = laid.files->Read(laid.root / relative);
    return contents ? contents.GetValue() : std::string{"<missing>"};
}

[[nodiscard]] bool Mentions(const std::vector<std::string>& warnings, std::string_view text)
{
    for (const std::string& warning : warnings)
    {
        if (warning.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("ModLayout: lays out candidates, metas and faux packs", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod =
        WriteMod(dir, "mycar",
                 {Add({"update\\update.rpf", "x64\\models.rpf"}, "car.yft", "car.yft"),
                  Add({"common.rpf"}, "handling.meta", "data\\handling.meta"),
                  Add({"common.rpf"}, "custom.meta", "data\\custom.meta"),
                  Add({"common.rpf"}, "extra.rpf", "extra.rpf"), Add({}, "extra.rpf", "dlc.rpf")});

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(laid.root == dir.Path() / "mods" / "mycar");
    CHECK(laid.files->GetFiles().size() == 5); // four files and the manifest
    CHECK(Read(laid, "stream/car.yft") == "model");
    CHECK(Read(laid, "stream/inner.ytd") == "nested");
    CHECK(Read(laid, "common/data/handling.meta") == "handling");
    CHECK(Read(laid, "common/data/custom.meta") == "custom"); // overlay only
    const std::string manifest = Read(laid, "fxmanifest.lua");
    CHECK(manifest.find("game 'gta5'") != std::string::npos);
    CHECK(manifest.find("data_file 'HANDLING_FILE' 'common/data/handling.meta'") !=
          std::string::npos);
    CHECK(manifest.find("custom.meta") == std::string::npos);
    CHECK(Mentions(laid.warnings, "setup2.xml"));
    CHECK(laid.warnings.size() == 1);

    REQUIRE(laid.overlays.size() == 2);
    CHECK(laid.overlays[0].folder == dir.Path() / "mods" / "mycar" / "common");
    CHECK(laid.overlays[0].mountPoint == "common:/");
    CHECK(laid.overlays[0].probeFile == "data/handling.meta");
    CHECK(laid.overlays[1].mountPoint == "commoncrc:/");
}

TEST_CASE("ModLayout: platform targets get their own overlay mounts", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod =
        WriteMod(dir, "hud", {Add({"update\\update.rpf"}, "one.ytd", "x64\\textures\\hud.ytd")});

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "platform/textures/hud.ytd") == "one");
    REQUIRE(laid.overlays.size() == 2);
    CHECK(laid.overlays[0].mountPoint == "platform:/");
    CHECK(laid.overlays[1].mountPoint == "platformcrc:/");
    CHECK(laid.overlays[0].probeFile == "textures/hud.ytd");
}

TEST_CASE("ModLayout: the first basename wins", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod =
        WriteMod(dir, "clash",
                 {Add({"update\\update.rpf", "x64\\a.rpf"}, "one.ytd", "same.ytd"),
                  Add({"update\\update.rpf", "x64\\b.rpf"}, "two.ytd", "same.ytd")});

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "stream/same.ytd") == "one");
    CHECK(Mentions(laid.warnings, "already provided"));
}

TEST_CASE("ModLayout: basenames that differ only in case clash too", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod =
        WriteMod(dir, "clash",
                 {Add({"update\\update.rpf", "x64\\a.rpf"}, "one.ytd", "Same.ytd"),
                  Add({"update\\update.rpf", "x64\\b.rpf"}, "two.ytd", "same.ytd")});

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "stream/same.ytd") == "one");
    CHECK(Mentions(laid.warnings, "already provided"));
}

TEST_CASE("ModLayout: resolves bare backslash sources under content/", "[mods]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/timecycle/timecycle_mods_1.xml", "<mods/>");
    dir.WriteFile("quant.rpf", builder.Build());

    // Real OpenIV shape (QuantV): a bare backslash source under content/.
    ModPackage package;
    package.entries = {Add({"update\\update.rpf"}, "timecycle\\timecycle_mods_1.xml",
                           "common\\data\\timecycle\\timecycle_mods_1.xml")};
    const DiscoveredMod mod{
        .name = "quant", .absolutePath = dir.Path() / "quant.rpf", .package = std::move(package)};
    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "common/data/timecycle/timecycle_mods_1.xml") == "<mods/>");
    const std::string manifest = Read(laid, "fxmanifest.lua");
    CHECK(manifest.find(
              "data_file 'TIMECYCLEMOD_FILE' 'common/data/timecycle/timecycle_mods_1.xml'") !=
          std::string::npos);
    CHECK(laid.warnings.empty());
}

TEST_CASE("ModLayout: sanitizes the mod folder name", "[mods]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    dir.WriteFile("mymod.rpf", builder.Build());

    // Archive stems are user input; the mod folder must stay a plain VFS path segment.
    const DiscoveredMod mod{
        .name = "my:mod", .absolutePath = dir.Path() / "mymod.rpf", .package = ModPackage{}};
    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(laid.root == dir.Path() / "mods" / "my_mod");
    CHECK(laid.files->GetFiles().size() == 1); // the manifest alone
}

namespace
{
/// A content DLC: setup2.xml descriptor plus a content.xml with one pack and one meta.
[[nodiscard]] std::string DlcArchive(std::string_view device, std::string_view order,
                                     std::string_view requiredVersion)
{
    RpfBuilder inner;
    inner.AddFile("top.ytd", "packed");
    inner.AddFile("sub/nested.ytd", "nested");

    RpfBuilder dlc;
    dlc.AddFile("setup2.xml",
                "<SSetupData><deviceName>" + std::string{device} + "</deviceName><order value=\"" +
                    std::string{order} + "\"/>" +
                    (requiredVersion.empty() ? ""
                                             : "<requiredVersion>" + std::string{requiredVersion} +
                                                   "</requiredVersion>") +
                    "</SSetupData>");
    dlc.AddFile("content.xml", "<CDataFileMgr__ContentsOfDataFileXml><dataFiles>"
                               "<Item><filename>" +
                                   std::string{device} + ":/%PLATFORM%/pack.rpf</filename>" +
                                   "<fileType>RPF_FILE</fileType></Item>"
                                   "<Item><filename>common/data/vehicles.meta</filename>"
                                   "<fileType>VEHICLE_METADATA_FILE</fileType></Item>"
                                   "</dataFiles></CDataFileMgr__ContentsOfDataFileXml>");
    dlc.AddFile("x64/pack.rpf", inner.Build());
    dlc.AddFile("common/data/vehicles.meta", "vehicles");
    return dlc.Build();
}
} // namespace

TEST_CASE("ModLayout: loads content DLCs in order", "[mods]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/second.rpf", DlcArchive("dlc_two", "20", ""));
    builder.AddFile("content/first.rpf", DlcArchive("dlc_one", "5", ""));
    dir.WriteFile("patch.rpf", builder.Build());

    // Assembly order is irrelevant; the DLC order attribute decides.
    ModPackage package;
    package.entries = {Add({}, "second.rpf", "second.rpf"), Add({}, "first.rpf", "first.rpf")};
    const DiscoveredMod mod{
        .name = "patch", .absolutePath = dir.Path() / "patch.rpf", .package = std::move(package)};
    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "stream/top.ytd") == "packed");
    CHECK(Read(laid, "__dlc__/dlc_one/common/data/vehicles.meta") == "vehicles");
    const std::string manifest = Read(laid, "fxmanifest.lua");
    const std::size_t one = manifest.find("__dlc__/dlc_one/");
    const std::size_t two = manifest.find("__dlc__/dlc_two/");
    REQUIRE(one != std::string::npos);
    REQUIRE(two != std::string::npos);
    CHECK(one < two); // order 5 before order 20
    // Both DLCs ship the same top.ytd: the first keeps it, and a pack repeating a name of an
    // earlier pack of the same mod is expected, not a warning.
    CHECK(laid.warnings.empty());
}

TEST_CASE("ModLayout: a file name with a line break keeps the manifest valid", "[mods]")
{
    TempDir dir;
    RpfBuilder dlc;
    dlc.AddFile("setup2.xml", "<SSetupData><deviceName>dlc_odd</deviceName></SSetupData>");
    dlc.AddFile("content.xml", "<CDataFileMgr__ContentsOfDataFileXml><dataFiles>"
                               "<Item><filename>common/data/odd\nname.meta</filename>"
                               "<fileType>VEHICLE_METADATA_FILE</fileType></Item>"
                               "</dataFiles></CDataFileMgr__ContentsOfDataFileXml>");
    dlc.AddFile("common/data/odd\nname.meta", "vehicles");
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/odd.rpf", dlc.Build());
    dir.WriteFile("patch.rpf", builder.Build());

    ModPackage package;
    package.entries = {Add({}, "odd.rpf", "odd.rpf")};
    const DiscoveredMod mod{
        .name = "patch", .absolutePath = dir.Path() / "patch.rpf", .package = std::move(package)};
    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    const ParseResult parsed =
        ManifestParser::Parse(Read(laid, "fxmanifest.lua"), "fxmanifest.lua");
    CHECK_FALSE(parsed.fatal);
    const std::vector<const ManifestEntry*> extras = parsed.document.GetEntries("data_file_extra");
    REQUIRE(extras.size() == 1);
    CHECK(extras.front()->decoded ==
          std::vector<std::string>{"__dlc__/dlc_odd/common/data/odd\nname.meta"});
}

TEST_CASE("ModLayout: skips DLCs outside their required version", "[mods]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/new.rpf", DlcArchive("dlc_new", "1", "9999-"));
    builder.AddFile("content/old.rpf", DlcArchive("dlc_old", "1", ""));
    dir.WriteFile("patch.rpf", builder.Build());

    ModPackage package;
    package.entries = {Add({}, "new.rpf", "new.rpf"), Add({}, "old.rpf", "old.rpf")};
    const DiscoveredMod mod{
        .name = "patch", .absolutePath = dir.Path() / "patch.rpf", .package = std::move(package)};
    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    const std::string manifest = Read(laid, "fxmanifest.lua");
    CHECK(manifest.find("dlc_new") == std::string::npos);
    CHECK(manifest.find("__dlc__/dlc_old/") != std::string::npos);
    CHECK(Mentions(laid.warnings, "9999-"));
}

TEST_CASE("ModLayout: every nested pack keeps its own packfile manifest", "[mods]")
{
    RpfBuilder first;
    first.AddFile("_manifest.ymf", "PSIN-first");
    first.AddFile("tree.ydr", "first-tree");
    RpfBuilder second;
    second.AddFile("_manifest.ymf", "PSIN-second");
    second.AddFile("tree.ydr", "second-tree");

    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/a.rpf", first.Build());
    builder.AddFile("content/b.rpf", second.Build());
    TempDir dir;
    dir.WriteFile("veg.rpf", builder.Build());

    ModPackage package;
    package.entries = {Add({"update\\update.rpf"}, "a.rpf", "x64\\levels\\a.rpf"),
                       Add({"update\\update.rpf"}, "b.rpf", "x64\\levels\\b.rpf")};
    const DiscoveredMod mod{
        .name = "veg", .absolutePath = dir.Path() / "veg.rpf", .package = std::move(package)};

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "stream/pack1/_manifest.ymf") == "PSIN-first");
    CHECK(Read(laid, "stream/pack2/_manifest.ymf") == "PSIN-second");
    CHECK(Read(laid, "stream/tree.ydr") == "first-tree");
    CHECK(laid.warnings.empty()); // a later pack repeating a name is not the user's problem
}

namespace
{
/// One half of a DLC split into dlc.rpf and dlc1.rpf: both halves share the device and the
/// content.xml, which lists every packfile of both and a type request for a .ytyp in b.rpf,
/// ahead of the packfile that holds it.
[[nodiscard]] std::string SplitDlcHalf(bool first)
{
    RpfBuilder dlc;
    dlc.AddFile("setup2.xml", "<SSetupData><deviceName>dlc_split</deviceName><order value=\"1\"/>"
                              "</SSetupData>");
    dlc.AddFile("content.xml", "<CDataFileMgr__ContentsOfDataFileXml><dataFiles>"
                               "<Item><filename>dlc_split:/%PLATFORM%/props.ityp</filename>"
                               "<fileType>DLC_ITYP_REQUEST</fileType></Item>"
                               "<Item><filename>dlc_split:/%PLATFORM%/game.ityp</filename>"
                               "<fileType>DLC_ITYP_REQUEST</fileType></Item>"
                               "<Item><filename>dlc_split:/%PLATFORM%/a.rpf</filename>"
                               "<fileType>RPF_FILE</fileType></Item>"
                               "<Item><filename>dlc_split:/%PLATFORM%/b.rpf</filename>"
                               "<fileType>RPF_FILE</fileType></Item>"
                               "</dataFiles></CDataFileMgr__ContentsOfDataFileXml>");
    if (first)
    {
        RpfBuilder a;
        a.AddFile("_manifest.ymf", "PSIN-a");
        a.AddFile("rock.ydr", "rock");
        dlc.AddFile("x64/a.rpf", a.Build());
    }
    else
    {
        RpfBuilder b;
        b.AddFile("_manifest.ymf", "PSIN-b");
        b.AddFile("props.ytyp", "types");
        dlc.AddFile("x64/b.rpf", b.Build());
    }
    return dlc.Build();
}

[[nodiscard]] DiscoveredMod WriteSplitMod(const TempDir& dir, std::string_view name, bool first,
                                          bool second)
{
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    ModPackage package;
    if (first)
    {
        builder.AddFile("content/dlc.rpf", SplitDlcHalf(true));
        package.entries.push_back(Add({}, "dlc.rpf", "dlc.rpf"));
    }
    if (second)
    {
        builder.AddFile("content/dlc1.rpf", SplitDlcHalf(false));
        package.entries.push_back(Add({}, "dlc1.rpf", "dlc1.rpf"));
    }
    const std::string file = std::string{name} + ".rpf";
    dir.WriteFile(file, builder.Build());
    return DiscoveredMod{.name = std::string{name},
                         .absolutePath = dir.Path() / file,
                         .package = std::move(package)};
}
} // namespace

TEST_CASE("ModLayout: a DLC split into dlc.rpf and dlc1.rpf resolves across both", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod = WriteSplitMod(dir, "split", true, true);

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(laid.warnings.empty());
    CHECK(laid.unresolvedDlcFiles.empty());
    CHECK(Read(laid, "stream/rock.ydr") == "rock");
    CHECK(Read(laid, "stream/props.ytyp") == "types");
    // Both content.xml files list both packfiles: each is laid once.
    CHECK(Read(laid, "stream/pack1/_manifest.ymf") == "PSIN-a");
    CHECK(Read(laid, "stream/pack2/_manifest.ymf") == "PSIN-b");
    CHECK_FALSE(laid.files->IsDirectory("stream/pack3"));
    CHECK(ModLayout::ReportUnresolvedDlcFiles(std::span{&laid, 1}).empty());

    const std::string manifest = Read(laid, "fxmanifest.lua");
    const std::string request = "data_file 'DLC_ITYP_REQUEST' 'stream/props.ytyp'";
    const std::size_t found = manifest.find(request);
    REQUIRE(found != std::string::npos);
    CHECK(manifest.find(request, found + 1) == std::string::npos);
    // A request no pack answers still goes to the streaming plan, which knows the game's types.
    CHECK(manifest.find("data_file 'DLC_ITYP_REQUEST' 'game.ytyp'") != std::string::npos);
    CHECK(manifest.find(".ityp") == std::string::npos);
}

TEST_CASE("ModLayout: a DLC split across two mods is only missing files neither has", "[mods]")
{
    TempDir dir;
    const DiscoveredMod part1 = WriteSplitMod(dir, "part1", true, false);
    const DiscoveredMod part2 = WriteSplitMod(dir, "part2", false, true);

    const std::vector<ModLayout::Result> both{ModLayout::Build(part1, dir.Path() / "mods", 3411),
                                              ModLayout::Build(part2, dir.Path() / "mods", 3411)};

    CHECK(both[0].warnings.empty());
    CHECK(both[1].warnings.empty());
    REQUIRE(both[0].unresolvedDlcFiles.size() == 1);
    CHECK(both[0].unresolvedDlcFiles[0].path == "x64/b.rpf");
    CHECK(ModLayout::ReportUnresolvedDlcFiles(both).empty());

    // Without the other half, the file is reported once, by its content.xml name.
    const std::vector<std::string> alone =
        ModLayout::ReportUnresolvedDlcFiles(std::span{both.data(), 1});
    REQUIRE(alone.size() == 1);
    CHECK(Mentions(alone, "dlc_split:/%PLATFORM%/b.rpf"));
}

TEST_CASE("ModLayout: nothing is written to disk", "[mods]")
{
    TempDir dir;
    const DiscoveredMod mod =
        WriteMod(dir, "mycar",
                 {Add({"update\\update.rpf", "x64\\models.rpf"}, "car.yft", "car.yft"),
                  Add({"common.rpf"}, "handling.meta", "data\\handling.meta")});

    const ModLayout::Result laid = ModLayout::Build(mod, dir.Path() / "mods", 3411);

    CHECK(Read(laid, "stream/car.yft") == "model");
    CHECK_FALSE(std::filesystem::exists(dir.Path() / "mods"));
}
