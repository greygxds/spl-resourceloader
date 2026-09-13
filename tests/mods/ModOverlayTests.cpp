#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "mods/ModOverlay.h"
#include "mods/ModPackage.h"

using spl::mods::ModEntry;
using spl::mods::ModOverlay;
using spl::mods::ModPackage;

namespace
{
[[nodiscard]] ModPackage PackageWith(std::vector<ModEntry> entries)
{
    ModPackage package;
    package.guid = "62AB8F34-BE20-46D5-9F0F-84729F087E5E";
    package.entries = std::move(entries);
    return package;
}

[[nodiscard]] ModEntry Add(std::vector<std::string> roots, std::string source, std::string target)
{
    return ModEntry{.archiveRoots = std::move(roots),
                    .sourceFile = std::move(source),
                    .targetFile = std::move(target)};
}

[[nodiscard]] bool HasMapping(const ModOverlay& overlay, std::string_view target,
                              std::string_view source)
{
    for (const auto& mapping : overlay.Mappings())
    {
        if (mapping.target == target && mapping.source == source)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasCandidate(const ModOverlay& overlay, std::string_view source)
{
    for (const auto& candidate : overlay.StreamCandidates())
    {
        if (candidate.source == source)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("ModOverlay: rewrites update.rpf targets onto platform", "[mods]")
{
    const ModOverlay overlay = ModOverlay::Build(
        PackageWith({Add({"update\\update.rpf"}, "content\\a.ytd", "x64\\textures\\a.ytd"),
                     Add({"update\\update.rpf"}, "content\\b.dat", "common\\data\\b.dat"),
                     Add({"update\\update.rpf"}, "content\\c.dat", "dlc_patch\\c.dat")}));

    CHECK(HasMapping(overlay, "platform/textures/a.ytd", "content/a.ytd"));
    CHECK(HasMapping(overlay, "common/data/b.dat", "content/b.dat"));
    CHECK(overlay.Mappings().size() == 2);          // dlc_patch/ is dropped
    CHECK(!HasCandidate(overlay, "content/a.ytd")); // depth 1, not textures/, not bare
}

TEST_CASE("ModOverlay: rewrites x64 and common archives", "[mods]")
{
    const ModOverlay overlay =
        ModOverlay::Build(PackageWith({Add({"x64A.rpf"}, "content\\a.ytd", "a.ytd"),
                                       Add({"common.rpf"}, "content\\b.ymap", "b.ymap"),
                                       Add({"patch\\day.rpf"}, "content\\c.ytd", "c.ytd")}));

    CHECK(HasMapping(overlay, "platform/a.ytd", "content/a.ytd"));
    CHECK(HasMapping(overlay, "common/b.ymap", "content/b.ymap"));
    CHECK(overlay.Mappings().size() == 2); // unknown roots are ignored
    CHECK(overlay.MapTargets().size() == 1);
    CHECK(overlay.MapTargets()[0].target == "common/b.ymap");
}

TEST_CASE("ModOverlay: skips denied targets and nested archives", "[mods]")
{
    const ModOverlay overlay = ModOverlay::Build(
        PackageWith({Add({"common.rpf"}, "content\\g.xml", "common/data/gameconfig.xml"),
                     Add({"update\\update.rpf"}, "content\\e.rpf", "extra.rpf"),
                     Add({}, "content\\dlc.rpf", "dlc.rpf")}));

    CHECK(overlay.Mappings().empty());
    REQUIRE(overlay.FauxPackJobs().size() == 1);
    CHECK(overlay.FauxPackJobs()[0].source == "content/e.rpf");
    REQUIRE(overlay.DlcJobs().size() == 1);
    CHECK(overlay.DlcJobs()[0].source == "content/dlc.rpf");
}

TEST_CASE("ModOverlay: normalizes slashes and strips one leading slash", "[mods]")
{
    const ModOverlay overlay =
        ModOverlay::Build(PackageWith({Add({"common.rpf"}, "content\\a.ytd", "/a.ytd")}));

    CHECK(HasMapping(overlay, "common/a.ytd", "content/a.ytd"));
    CHECK(overlay.FindSource("common/a.ytd") == "content/a.ytd");
    CHECK(overlay.FindSource("COMMON/A.YTD") == "content/a.ytd");
    CHECK(!overlay.FindSource("common/other.ytd").has_value());
}

TEST_CASE("ModOverlay: applies the streaming predicate", "[mods]")
{
    const ModOverlay overlay = ModOverlay::Build(PackageWith({
        Add({"u.rpf", "x.rpf"}, "content\\deep.ytd", "deep.ytd"),     // depth 2, bare
        Add({"u.rpf"}, "content\\shallow.ytd", "shallow.ytd"),        // depth 1, bare
        Add({"u.rpf"}, "content\\t.ytd", "textures\\t.ytd"),          // depth 1, core texture
        Add({"u.rpf", "x.rpf"}, "content\\s.ytd", "sub/s.ytd"),       // depth 2, slashed
        Add({"u.rpf", "x.rpf"}, "content\\t2.ytd", "textures/t2.ytd") // depth 2, core texture
    }));

    CHECK(HasCandidate(overlay, "content/deep.ytd"));
    CHECK(!HasCandidate(overlay, "content/shallow.ytd"));
    CHECK(HasCandidate(overlay, "content/t.ytd"));
    CHECK(!HasCandidate(overlay, "content/s.ytd"));
    CHECK(HasCandidate(overlay, "content/t2.ytd"));
}

TEST_CASE("ModOverlay: collects meta and manifest targets", "[mods]")
{
    const ModOverlay overlay =
        ModOverlay::Build(PackageWith({Add({"common.rpf"}, "content\\h.meta", "handling.meta"),
                                       Add({"common.rpf"}, "content\\m.ymf", "m.ymf"),
                                       Add({"common.rpf"}, "content\\a.ytd", "a.ytd")}));

    REQUIRE(overlay.MetaFiles().size() == 1);
    CHECK(overlay.MetaFiles()[0].target == "common/handling.meta");
    REQUIRE(overlay.ManifestTargets().size() == 1);
    CHECK(overlay.ManifestTargets()[0].target == "common/m.ymf");
}
