#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "core/LoadSummary.h"
#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "streaming/AssetRegistry.h"
#include "streaming/StreamingPlan.h"
#include "tests/streaming/StreamTree.h"

using spl::BuildLoadSummary;
using spl::LoadSummary;
using spl::LoadSummaryInput;
using spl::OverlayMount;
using spl::resource::Resource;
using spl::resource::ResourceCandidate;
using spl::resource::ResourceId;
using spl::resource::ResourceState;
using spl::streaming::AssetRegistry;
using spl::streaming::LoadedDataFile;
using spl::streaming::RegisteredAsset;
using spl::streaming::StreamingPlan;
using spl::tests::StreamTree;

namespace
{
constexpr uint32_t kDrawableVersion = 165;

/// What the pump would record for a registration.
void Register(AssetRegistry& registry, uint16_t owner, std::string fileName, bool overrides)
{
    registry.Add(RegisteredAsset{.owner = ResourceId{owner},
                                 .resourceName = "",
                                 .fileName = std::move(fileName),
                                 .overridesGameAsset = overrides});
}

Resource MakeMod(StreamTree& tree, std::string_view name)
{
    Resource resource = tree.AddStreamResource(name);
    return Resource{ResourceCandidate{.name = std::string{name},
                                      .root = resource.GetRootPath(),
                                      .manifestPath = resource.GetManifestPath(),
                                      .isMod = true}};
}
} // namespace

TEST_CASE("LoadSummary: one line per resource and per mod, each group with its totals", "[core]")
{
    StreamTree tree;
    tree.AddAsset("props", "a.ydr", kDrawableVersion);
    tree.AddAsset("props", "b.ydr", kDrawableVersion);
    tree.AddAsset("quant", "water.ydr", kDrawableVersion);

    spl::manifest::ResourceManifest manifest;
    manifest.ignoredScriptEntries = 1;
    manifest.dataFiles.push_back(spl::manifest::DataFileEntry{
        .type = "TEXTFILE_METAFILE", .pattern = "dlctext.meta", .resolved = {"dlctext.meta"}});

    std::vector<Resource> resources;
    resources.push_back(spl::tests::WithManifest(tree.AddStreamResource("props"), manifest));
    Resource disabled = tree.AddStreamResource("cayo");
    disabled.SetState(ResourceState::Disabled, "disabled by configuration");
    resources.push_back(std::move(disabled));
    resources.push_back(MakeMod(tree, "quant"));

    const StreamingPlan plan = StreamingPlan::Build(resources, spl::config::StreamingSettings{},
                                                    spl::config::DiagnosticsSettings{});
    AssetRegistry registry;
    Register(registry, 0, "a.ydr", false);
    Register(registry, 0, "b.ydr", true);
    Register(registry, 2, "water.ydr", true);
    const std::vector<OverlayMount> overlays = {
        {.folder = resources[2].GetRootPath() / "common", .mountPoint = "common:/"},
        {.folder = resources[2].GetRootPath() / "common", .mountPoint = "commoncrc:/"},
        {.folder = resources[2].GetRootPath() / "platform", .mountPoint = "platform:/"}};

    const LoadSummary summary = BuildLoadSummary(LoadSummaryInput{
        .resources = resources, .plan = &plan, .registry = &registry, .overlays = overlays});

    REQUIRE(summary.resources.size() == 2);
    CHECK(summary.resources[0] == "props: 2 assets (1 replaces a game file), 1 data file refused "
                                  "as in FiveM, 1 script entry ignored");
    CHECK(summary.resources[1] == "cayo: disabled by configuration");
    CHECK(summary.resourceTotals ==
          "Resources: 1 of 2 loaded, 2 assets (1 replaces a game file), 0 data files");

    REQUIRE(summary.mods.size() == 1);
    CHECK(summary.mods[0] == "quant: 1 file streamed (1 replaces a game file), replaces game files "
                             "in common:/ and platform:/");
    CHECK(summary.modTotals == "Mods: 1 of 1 loaded, 1 file streamed (1 replaces a game file), 0 "
                               "data files, 1 mod replacing game files in place");
}

TEST_CASE("LoadSummary: what did not load is counted, and an empty group has no lines", "[core]")
{
    StreamTree tree;
    tree.AddAsset("props", "a.ydr", kDrawableVersion);
    tree.AddAsset("props", "b.ydr", kDrawableVersion);
    tree.AddFile("props", "broken.ydr", "not compiled");

    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("props"));
    const StreamingPlan plan = StreamingPlan::Build(resources, spl::config::StreamingSettings{},
                                                    spl::config::DiagnosticsSettings{});
    AssetRegistry registry;
    Register(registry, 0, "a.ydr", false);
    registry.AddDataFile(LoadedDataFile{.owner = ResourceId{0}, .type = "HANDLING_FILE"});

    const LoadSummary summary = BuildLoadSummary(
        LoadSummaryInput{.resources = resources, .plan = &plan, .registry = &registry});

    REQUIRE(summary.resources.size() == 1);
    CHECK(summary.resources[0] == "props: 1 asset, 1 not registered, 1 invalid, 1 data file");
    CHECK(summary.resourceTotals ==
          "Resources: 1 of 1 loaded, 1 asset, 1 not registered, 1 data file");
    CHECK(summary.mods.empty());
    CHECK(summary.modTotals.empty());
}
