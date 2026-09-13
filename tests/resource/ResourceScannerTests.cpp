#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "resource/Resource.h"
#include "resource/ResourceScanner.h"
#include "tests/TempTree.h"

using spl::resource::ManifestKind;
using spl::resource::ResourceCandidate;
using spl::resource::ResourceScanner;
using spl::tests::TempTree;

namespace
{
std::vector<std::string> NamesOf(const std::vector<ResourceCandidate>& candidates)
{
    std::vector<std::string> names;
    names.reserve(candidates.size());
    for (const ResourceCandidate& candidate : candidates)
    {
        names.push_back(candidate.name);
    }
    return names;
}

bool Mentions(const std::vector<std::string>& messages, std::string_view text)
{
    return std::ranges::any_of(messages, [&](const std::string& message)
                               { return message.find(text) != std::string::npos; });
}
} // namespace

TEST_CASE("ResourceScanner: finds resources in case-insensitive name order", "[resource]")
{
    TempTree tree;
    tree.AddResource("b_second");
    tree.AddResource("A_first");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(NamesOf(result.resources) == std::vector<std::string>{"A_first", "b_second"});
    REQUIRE(result.warnings.empty());
}

TEST_CASE("ResourceScanner: recurses into categories and records them", "[resource]")
{
    TempTree tree;
    tree.AddResource("[maps]/city");
    tree.AddResource("[maps]/[old]/village");
    tree.AddResource("standalone");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.size() == 3);
    const auto city = std::ranges::find_if(result.resources, [](const ResourceCandidate& candidate)
                                           { return candidate.name == "city"; });
    const auto village =
        std::ranges::find_if(result.resources, [](const ResourceCandidate& candidate)
                             { return candidate.name == "village"; });
    const auto standalone =
        std::ranges::find_if(result.resources, [](const ResourceCandidate& candidate)
                             { return candidate.name == "standalone"; });

    REQUIRE(city != result.resources.end());
    REQUIRE(village != result.resources.end());
    REQUIRE(standalone != result.resources.end());
    REQUIRE(city->categories == std::vector<std::string>{"[maps]"});
    REQUIRE(village->categories == std::vector<std::string>{"[maps]", "[old]"});
    REQUIRE(standalone->categories.empty());
}

TEST_CASE("ResourceScanner: a category is never a resource itself", "[resource]")
{
    TempTree tree;
    tree.AddResource("[maps]"); // manifest directly inside the category folder
    tree.AddResource("[maps]/city");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(NamesOf(result.resources) == std::vector<std::string>{"city"});
    REQUIRE(Mentions(result.warnings, "is a category but has a resource manifest"));
}

TEST_CASE("ResourceScanner: a folder without a manifest is skipped, not descended into",
          "[resource]")
{
    TempTree tree;
    tree.AddDirectory("not_a_resource");
    tree.AddResource("not_a_resource/buried"); // must not be found: no manifest above it

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.empty());
}

TEST_CASE("ResourceScanner: a resource inside a resource is not discovered", "[resource]")
{
    TempTree tree;
    tree.AddResource("outer");
    tree.AddResource("outer/inner");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(NamesOf(result.resources) == std::vector<std::string>{"outer"});
}

TEST_CASE("ResourceScanner: legacy manifests depend on the flag", "[resource]")
{
    TempTree tree;
    tree.AddLegacyResource("old_style");

    const ResourceScanner::Result accepted = ResourceScanner::Scan(tree.Root(), true);
    REQUIRE(NamesOf(accepted.resources) == std::vector<std::string>{"old_style"});
    REQUIRE(accepted.resources.front().manifestKind == ManifestKind::LegacyResourceLua);

    const ResourceScanner::Result rejected = ResourceScanner::Scan(tree.Root(), false);
    REQUIRE(rejected.resources.empty());
}

TEST_CASE("ResourceScanner: fxmanifest wins when both manifests exist", "[resource]")
{
    TempTree tree;
    tree.AddResource("both");
    tree.WriteFile("both/__resource.lua", "-- legacy\n");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.size() == 1);
    REQUIRE(result.resources.front().manifestKind == ManifestKind::FxManifest);
    REQUIRE(result.resources.front().manifestPath.filename() == "fxmanifest.lua");
}

TEST_CASE("ResourceScanner: duplicate names keep the first in sorted order", "[resource]")
{
    TempTree tree;
    // Created in reverse, so a result that depended on enumeration order would come out wrong.
    tree.AddResource("[old]/foo");
    tree.AddResource("[maps]/foo");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.size() == 1);
    REQUIRE(result.resources.front().categories == std::vector<std::string>{"[maps]"});
    REQUIRE(Mentions(result.warnings, "Duplicate resource 'foo'"));
}

TEST_CASE("ResourceScanner: duplicate detection ignores case", "[resource]")
{
    TempTree tree;
    tree.AddResource("[a]/Foo");
    tree.AddResource("[b]/FOO");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.size() == 1);
    REQUIRE(Mentions(result.warnings, "Duplicate resource"));
}

TEST_CASE("ResourceScanner: hidden folders are ignored", "[resource]")
{
    TempTree tree;
    tree.AddResource(".git");
    tree.AddResource(".hidden");
    tree.AddResource("visible");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(NamesOf(result.resources) == std::vector<std::string>{"visible"});
}

TEST_CASE("ResourceScanner: an unusual name loads but is reported", "[resource]")
{
    TempTree tree;
    tree.AddResource("my resource!");

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(NamesOf(result.resources) == std::vector<std::string>{"my resource!"});
    REQUIRE(Mentions(result.warnings, "my resource!"));
}

TEST_CASE("ResourceScanner: a missing root yields nothing and one warning", "[resource]")
{
    TempTree tree;

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root() / "absent", true);

    REQUIRE(result.resources.empty());
    REQUIRE(result.warnings.size() == 1);
}

TEST_CASE("ResourceScanner: an empty root yields nothing and no warnings", "[resource]")
{
    TempTree tree;

    const ResourceScanner::Result result = ResourceScanner::Scan(tree.Root(), true);

    REQUIRE(result.resources.empty());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("ResourceScanner: IsCategoryName recognizes bracketed names", "[resource]")
{
    REQUIRE(spl::resource::IsCategoryName("[maps]"));
    REQUIRE(spl::resource::IsCategoryName("[]"));
    REQUIRE_FALSE(spl::resource::IsCategoryName("[maps"));
    REQUIRE_FALSE(spl::resource::IsCategoryName("maps]"));
    REQUIRE_FALSE(spl::resource::IsCategoryName("maps"));
    REQUIRE_FALSE(spl::resource::IsCategoryName(""));
}

TEST_CASE("ResourceScanner: IsPlainResourceName accepts the documented characters", "[resource]")
{
    REQUIRE(spl::resource::IsPlainResourceName("map_one-2.0"));
    REQUIRE_FALSE(spl::resource::IsPlainResourceName("map one"));
    REQUIRE_FALSE(spl::resource::IsPlainResourceName("map!"));
    REQUIRE_FALSE(spl::resource::IsPlainResourceName(""));
}
