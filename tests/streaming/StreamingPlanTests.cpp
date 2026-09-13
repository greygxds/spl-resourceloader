#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "streaming/AssetType.h"
#include "streaming/StreamAsset.h"
#include "streaming/StreamingPlan.h"
#include "tests/streaming/StreamTree.h"
#include "util/Glob.h"

using spl::config::DataFileSettings;
using spl::config::DiagnosticsSettings;
using spl::config::DuplicatePolicy;
using spl::config::StreamingSettings;
using spl::manifest::DataFileEntry;
using spl::manifest::ResourceManifest;
using spl::resource::Resource;
using spl::resource::ResourceState;
using spl::streaming::AssetDisposition;
using spl::streaming::AssetType;
using spl::streaming::DataFilePolicy;
using spl::streaming::IsSupportedDataFileType;
using spl::streaming::PlannedAsset;
using spl::streaming::ResourcePlan;
using spl::streaming::StreamAsset;
using spl::streaming::StreamingPlan;
using spl::tests::StreamTree;

namespace
{
StreamingPlan BuildPlan(std::vector<Resource>& resources,
                        const StreamingSettings& streaming = StreamingSettings{},
                        const DiagnosticsSettings& diagnostics = DiagnosticsSettings{},
                        const DataFileSettings& dataFiles = DataFileSettings{})
{
    return StreamingPlan::Build(resources, streaming, diagnostics, dataFiles);
}

std::vector<std::string> FileNamesOf(std::span<const PlannedAsset> assets)
{
    std::vector<std::string> names;
    names.reserve(assets.size());
    for (const PlannedAsset& asset : assets)
    {
        names.push_back(asset.fileName);
    }
    return names;
}

const StreamAsset* Find(const StreamingPlan& plan, std::string_view resourceName,
                        std::string_view fileName)
{
    const auto match = std::ranges::find_if(
        plan.Assets(), [&](const StreamAsset& asset)
        { return asset.resourceName == resourceName && asset.fileName == fileName; });
    return match != plan.Assets().end() ? &*match : nullptr;
}

const ResourcePlan* PlanFor(const StreamingPlan& plan, std::string_view resourceName)
{
    const auto match = std::ranges::find_if(plan.Resources(), [resourceName](const ResourcePlan& p)
                                            { return p.name == resourceName; });
    return match != plan.Resources().end() ? &*match : nullptr;
}
} // namespace

TEST_CASE("StreamingPlan: registers early types before late ones, in type order", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("example_map"));

    tree.AddAsset("example_map", "city.ymap", 2);
    tree.AddAsset("example_map", "city.ybn", 43);
    tree.AddAsset("example_map", "city.ytyp", 2);
    tree.AddAsset("example_map", "city.ytd", 13);
    tree.AddAsset("example_map", "prop.ydr", 165);
    tree.AddAsset("example_map", "pack.ymf", 0);

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Assets().size() == 6);
    REQUIRE(FileNamesOf(plan.Early()) == std::vector<std::string>{"city.ytd", "prop.ydr"});
    REQUIRE(FileNamesOf(plan.Late()) ==
            std::vector<std::string>{"city.ytyp", "city.ybn", "city.ymap"});
    REQUIRE(plan.Manifests().size() == 1);
    REQUIRE(plan.Manifests().front().fileName == "pack.ymf");
    REQUIRE(plan.CountOf(AssetDisposition::Planned) == 6);
    REQUIRE(plan.NeedsMapStoreReload());
    REQUIRE(resources.front().GetState() == ResourceState::Scanned);
}

TEST_CASE("StreamingPlan: counts and per-resource flags", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("textures_only"));
    tree.AddAsset("textures_only", "one.ytd", 13);
    tree.AddAsset("textures_only", "two.ytd", 13);

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.CountOf(AssetType::TextureDictionary) == 2);
    REQUIRE(plan.CountOf(AssetType::MapData) == 0);

    const ResourcePlan* resourcePlan = PlanFor(plan, "textures_only");
    REQUIRE(resourcePlan != nullptr);
    REQUIRE(resourcePlan->plannedAssets == 2);
    REQUIRE(resourcePlan->skippedAssets == 0);
    REQUIRE_FALSE(resourcePlan->needsMapStoreReload);
    REQUIRE_FALSE(resourcePlan->hasPackfileManifest);
    REQUIRE_FALSE(plan.NeedsMapStoreReload());
}

TEST_CASE("StreamingPlan: a manifest map needs a map store reload without ymaps", "[streaming]")
{
    StreamTree tree;
    ResourceManifest manifest;
    manifest.isMap = true;

    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("map_flag"), std::move(manifest)));
    tree.AddAsset("map_flag", "prop.ydr", 165);

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Late().empty());
    REQUIRE(plan.NeedsMapStoreReload());
}

TEST_CASE("StreamingPlan: a ymf sets hasPackfileManifest", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("with_manifest"));
    tree.AddPsoFile("with_manifest", "pack.ymf"); // what FiveM resources ship

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Late().empty()); // a ymf is never a streaming object
    REQUIRE(plan.Manifests().size() == 1);
    REQUIRE(PlanFor(plan, "with_manifest")->hasPackfileManifest);
}

TEST_CASE("StreamingPlan: closed config gates skip their types", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("everything"));
    tree.AddAsset("everything", "city.ytd", 13);
    tree.AddAsset("everything", "city.ytyp", 2);
    tree.AddAsset("everything", "city.ymap", 2);
    tree.AddAsset("everything", "city.ybn", 43);

    StreamingSettings streaming;
    streaming.loadMaps = false;

    const StreamingPlan plan = BuildPlan(resources, streaming);

    REQUIRE(FileNamesOf(plan.Early()) == std::vector<std::string>{"city.ytd"});
    REQUIRE(FileNamesOf(plan.Late()) == std::vector<std::string>{"city.ybn"});

    const StreamAsset* maps = Find(plan, "everything", "city.ymap");
    REQUIRE(maps != nullptr);
    REQUIRE(maps->disposition == AssetDisposition::SkippedByConfig);
    REQUIRE(maps->dispositionReason == "load_maps = false");
    REQUIRE(Find(plan, "everything", "city.ytyp")->disposition ==
            AssetDisposition::SkippedByConfig);
    REQUIRE(plan.CountOf(AssetDisposition::SkippedByConfig) == 2);
}

TEST_CASE("StreamingPlan: streaming disabled plans nothing", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("example"));
    tree.AddAsset("example", "city.ytd", 13);

    StreamingSettings streaming;
    streaming.enabled = false;

    const StreamingPlan plan = BuildPlan(resources, streaming);

    REQUIRE(plan.Assets().empty());
    REQUIRE(plan.Early().empty());
    REQUIRE(resources.front().GetState() == ResourceState::Discovered);
}

TEST_CASE("StreamingPlan: duplicate_policy = first keeps the earlier resource", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("map_one"));
    resources.push_back(tree.AddStreamResource("map_two"));
    tree.AddAsset("map_one", "prop_bench.ydr", 165);
    tree.AddAsset("map_two", "prop_bench.ydr", 165);

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Early().size() == 1);
    REQUIRE(plan.Early().front().resourceName == "map_one");
    REQUIRE(Find(plan, "map_two", "prop_bench.ydr")->disposition ==
            AssetDisposition::ShadowedByDuplicate);
    REQUIRE(Find(plan, "map_two", "prop_bench.ydr")->dispositionReason.find("map_one") !=
            std::string::npos);
    REQUIRE(PlanFor(plan, "map_two")->skippedAssets == 1);
}

TEST_CASE("StreamingPlan: every resource keeps its own _manifest.ymf", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("map_one"));
    resources.push_back(tree.AddStreamResource("map_two"));
    tree.AddPsoFile("map_one", "_manifest.ymf");
    tree.AddPsoFile("map_two", "_manifest.ymf");

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Manifests().size() == 2);
    REQUIRE(plan.Manifests()[0].resourceName == "map_one");
    REQUIRE(plan.Manifests()[1].resourceName == "map_two");
    REQUIRE(plan.CountOf(AssetDisposition::ShadowedByDuplicate) == 0);
    REQUIRE(PlanFor(plan, "map_one")->hasPackfileManifest);
    REQUIRE(PlanFor(plan, "map_two")->hasPackfileManifest);
}

TEST_CASE("StreamingPlan: duplicate_policy = last keeps the later resource", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("map_one"));
    resources.push_back(tree.AddStreamResource("map_two"));
    tree.AddAsset("map_one", "prop_bench.ydr", 165);
    tree.AddAsset("map_two", "prop_bench.ydr", 165);

    StreamingSettings streaming;
    streaming.duplicatePolicy = DuplicatePolicy::LastWins;

    const StreamingPlan plan = BuildPlan(resources, streaming);

    REQUIRE(plan.Early().size() == 1);
    REQUIRE(plan.Early().front().resourceName == "map_two");
    REQUIRE(Find(plan, "map_one", "prop_bench.ydr")->disposition ==
            AssetDisposition::ShadowedByDuplicate);
}

TEST_CASE("StreamingPlan: a duplicate inside one resource keeps the first path", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("map_one"));
    tree.AddAsset("map_one", "b_second/prop.ydr", 165);
    tree.AddAsset("map_one", "a_first/prop.ydr", 165);

    StreamingSettings streaming;
    streaming.duplicatePolicy = DuplicatePolicy::LastWins; // within a resource, order decides

    const StreamingPlan plan = BuildPlan(resources, streaming);

    REQUIRE(plan.Early().size() == 1);
    REQUIRE(plan.Early().front().relativePath == "stream/a_first/prop.ydr");

    const auto shadowed = std::ranges::find_if(
        plan.Assets(), [](const StreamAsset& asset)
        { return asset.disposition == AssetDisposition::ShadowedByDuplicate; });
    REQUIRE(shadowed != plan.Assets().end());
    REQUIRE(shadowed->relativePath == "stream/b_second/prop.ydr");
}

TEST_CASE("StreamingPlan: a skipped asset never shadows a usable one", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("broken_one"));
    resources.push_back(tree.AddStreamResource("good_two"));
    tree.AddFile("broken_one", "prop.ydr", "<Drawable>not compiled</Drawable>");
    tree.AddAsset("good_two", "prop.ydr", 165);

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.Early().size() == 1);
    REQUIRE(plan.Early().front().resourceName == "good_two");
    REQUIRE(Find(plan, "broken_one", "prop.ydr")->disposition == AssetDisposition::SkippedInvalid);
}

TEST_CASE("StreamingPlan: disabled resources are not scanned", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("enabled"));
    resources.push_back(tree.AddStreamResource("switched_off"));
    tree.AddAsset("enabled", "one.ytd", 13);
    tree.AddAsset("switched_off", "two.ytd", 13);
    resources.back().SetState(ResourceState::Disabled, "disabled by configuration");

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(FileNamesOf(plan.Early()) == std::vector<std::string>{"one.ytd"});
    REQUIRE(PlanFor(plan, "switched_off") == nullptr);
}

TEST_CASE("StreamingPlan: collects supported data files and reports the rest", "[streaming]")
{
    StreamTree tree;

    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "DLC_ITYP_REQUEST",
                                               .pattern = "props.ytyp",
                                               .resolved = {"props.ytyp"},
                                               .line = 4});
    manifest.dataFiles.push_back(DataFileEntry{.type = "TEXTFILE_METAFILE",
                                               .pattern = "dlctext.meta",
                                               .resolved = {"dlctext.meta"},
                                               .line = 5});
    manifest.dataFiles.push_back(DataFileEntry{.type = "NOT_A_GAME_TYPE",
                                               .pattern = "whatever.meta",
                                               .resolved = {"whatever.meta"},
                                               .line = 6});
    manifest.dataFiles.push_back(DataFileEntry{.type = "GTXD_PARENTING_DATA",
                                               .pattern = "gtxd.meta",
                                               .resolved = {"gtxd.meta"},
                                               .line = 7});

    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("with_data"), std::move(manifest)));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.DataFiles().size() == 1);
    REQUIRE(plan.DataFiles().front().type == "DLC_ITYP_REQUEST");
    REQUIRE(plan.DataFiles().front().policy == DataFilePolicy::TypeRequest);
    REQUIRE(plan.DataFiles().front().relativePath == "props.ytyp");
    REQUIRE(plan.DataFiles().front().absolutePath == tree.Root() / "with_data" / "props.ytyp");

    // FiveM mounts GTXD_PARENTING_DATA after the map store reload.
    REQUIRE(plan.DeferredDataFiles().size() == 1);
    REQUIRE(plan.DeferredDataFiles().front().type == "GTXD_PARENTING_DATA");
    REQUIRE(plan.DeferredDataFiles().front().policy == DataFilePolicy::Deferred);
}

TEST_CASE("StreamingPlan: RPF_FILE is planned as a packfile", "[streaming]")
{
    StreamTree tree;
    tree.WriteFile("dlc_pack/dlc.rpf", "RPF7");

    ResourceManifest manifest;
    manifest.dataFiles.push_back(
        DataFileEntry{.type = "RPF_FILE", .pattern = "dlc.rpf", .resolved = {"dlc.rpf"}});
    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("dlc_pack"), std::move(manifest)));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.DataFiles().size() == 1);
    REQUIRE(plan.DataFiles().front().policy == DataFilePolicy::Packfile);
    REQUIRE(plan.DeferredDataFiles().empty());
}

TEST_CASE("StreamingPlan: an @resource data file resolves in the other resource", "[streaming]")
{
    StreamTree tree;
    tree.WriteFile("shared_data/data/weapons/weapons.meta", "<CWeaponInfoBlob />");

    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "WEAPONINFO_FILE",
                                               .pattern = "data/**/weapons.meta",
                                               .otherResource = "Shared_Data"});
    manifest.dataFiles.push_back(DataFileEntry{
        .type = "WEAPONINFO_FILE", .pattern = "data/x.meta", .otherResource = "missing"});

    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("weapon"), std::move(manifest)));
    resources.push_back(tree.AddStreamResource("shared_data"));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.DataFiles().size() == 1);
    const spl::streaming::PlannedDataFile& dataFile = plan.DataFiles().front();
    REQUIRE(dataFile.resourceName == "weapon");
    REQUIRE(dataFile.relativePath == "@Shared_Data/data/weapons/weapons.meta");
    REQUIRE(dataFile.absolutePath == tree.Root() / "shared_data" / "data/weapons/weapons.meta");
    REQUIRE(dataFile.fileName == "weapons.meta");
}

TEST_CASE("StreamingPlan: every data file type the game knows is supported, as in FiveM",
          "[streaming]")
{
    REQUIRE(IsSupportedDataFileType("DLC_ITYP_REQUEST"));
    REQUIRE(IsSupportedDataFileType("dlc_ityp_request"));
    REQUIRE(IsSupportedDataFileType("HANDLING_FILE"));
    REQUIRE(IsSupportedDataFileType("WEAPONINFO_FILE"));
    REQUIRE(IsSupportedDataFileType("AUDIO_SOUNDDATA"));
    REQUIRE_FALSE(IsSupportedDataFileType("TEXTFILE_METAFILE"));
    REQUIRE(IsSupportedDataFileType("RPF_FILE"));
    REQUIRE(IsSupportedDataFileType("GTXD_PARENTING_DATA"));
    REQUIRE_FALSE(IsSupportedDataFileType("NOT_A_GAME_TYPE"));
    REQUIRE_FALSE(IsSupportedDataFileType(""));
}

namespace
{
/// The layout of a FiveM add-on car: metas in a folder with spaces, found through a ** glob.
Resource AddVehicleResource(StreamTree& tree, std::string_view name)
{
    tree.WriteFile(std::string{name} + "/data/Mclaren 720S GT3/vehicles.meta",
                   "<CVehicleModelInfo />");
    tree.WriteFile(std::string{name} + "/data/Mclaren 720S GT3/handling.meta",
                   "<CHandlingDataMgr />");
    tree.WriteFile(std::string{name} + "/data/Mclaren 720S GT3/carcols.meta",
                   "<CVehicleModelInfoVarGlobal />");

    ResourceManifest manifest;
    const auto add = [&](std::string_view type, std::string_view file)
    {
        const std::string pattern = "data/**/" + std::string{file};
        manifest.dataFiles.push_back(
            DataFileEntry{.type = std::string{type},
                          .pattern = pattern,
                          .resolved = spl::util::GlobFiles(tree.Root() / name, pattern)});
    };
    add("VEHICLE_METADATA_FILE", "vehicles.meta");
    add("CARCOLS_FILE", "carcols.meta");
    add("HANDLING_FILE", "handling.meta");
    return spl::tests::WithManifest(tree.AddStreamResource(name), std::move(manifest));
}

std::vector<std::string> TypesOf(const StreamingPlan& plan)
{
    std::vector<std::string> types;
    for (const auto& dataFile : plan.DataFiles())
    {
        types.push_back(dataFile.type);
    }
    return types;
}
} // namespace

TEST_CASE("StreamingPlan: vehicle metas are planned, handling first", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(AddVehicleResource(tree, "sg720sgt3"));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(TypesOf(plan) ==
            std::vector<std::string>{"HANDLING_FILE", "VEHICLE_METADATA_FILE", "CARCOLS_FILE"});
    const auto& handling = plan.DataFiles().front();
    CHECK(handling.policy == DataFilePolicy::Generic);
    CHECK(handling.relativePath == "data/Mclaren 720S GT3/handling.meta");
    CHECK(handling.absolutePath ==
          tree.Root() / "sg720sgt3" / "data/Mclaren 720S GT3/handling.meta");
    CHECK_FALSE(handling.matchesStreamedAsset);
}

TEST_CASE("StreamingPlan: handling loads before the metas of earlier resources", "[streaming]")
{
    StreamTree tree;
    ResourceManifest weapon;
    tree.WriteFile("baton/weapon_baton.meta", "<CWeaponInfoBlob />");
    weapon.dataFiles.push_back(DataFileEntry{.type = "WEAPONINFO_FILE",
                                             .pattern = "weapon_baton.meta",
                                             .resolved = {"weapon_baton.meta"}});

    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("baton"), std::move(weapon)));
    resources.push_back(AddVehicleResource(tree, "car"));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(TypesOf(plan) == std::vector<std::string>{"HANDLING_FILE", "WEAPONINFO_FILE",
                                                      "VEHICLE_METADATA_FILE", "CARCOLS_FILE"});
}

TEST_CASE("StreamingPlan: [data_files] switches keep types out", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(AddVehicleResource(tree, "car"));

    SECTION("everything")
    {
        const StreamingPlan plan = BuildPlan(resources, {}, {}, DataFileSettings{.enabled = false});
        CHECK(plan.DataFiles().empty());
    }
    SECTION("a category")
    {
        const StreamingPlan plan =
            BuildPlan(resources, {}, {}, DataFileSettings{.vehicles = false});
        CHECK(plan.DataFiles().empty());
    }
    SECTION("one type, in any case")
    {
        const StreamingPlan plan =
            BuildPlan(resources, {}, {}, DataFileSettings{.disabledTypes = {"carcols_file"}});
        CHECK(TypesOf(plan) == std::vector<std::string>{"HANDLING_FILE", "VEHICLE_METADATA_FILE"});
    }
}

TEST_CASE("StreamingPlan: an audio data file named without its suffix is planned", "[streaming]")
{
    StreamTree tree;
    tree.WriteFile("baton/audio/baton_game.dat151.rel", "rel");
    tree.WriteFile("baton/audio/baton_sounds.dat54.rel", "rel");

    // Literal patterns that match no file are kept as written by the manifest reader.
    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "AUDIO_GAMEDATA",
                                               .pattern = "audio/baton_game.dat",
                                               .resolved = {"audio/baton_game.dat"}});
    manifest.dataFiles.push_back(DataFileEntry{.type = "AUDIO_SOUNDDATA",
                                               .pattern = "audio/baton_sounds.dat",
                                               .resolved = {"audio/baton_sounds.dat"}});
    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("baton"), std::move(manifest)));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(TypesOf(plan) == std::vector<std::string>{"AUDIO_GAMEDATA", "AUDIO_SOUNDDATA"});
    CHECK(plan.DataFiles()[0].relativePath == "audio/baton_game.dat");
}

TEST_CASE("StreamingPlan: an obsolete assault_vehicles pack loads no data files", "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(AddVehicleResource(tree, "old_pack"));

    ResourceManifest manifest = *resources.front().GetManifest();
    manifest.dataFiles.push_back(
        DataFileEntry{.type = "VEHICLE_METADATA_FILE",
                      .pattern = "data/ai/vehicleweapons_caracara.meta",
                      .resolved = {"data/ai/vehicleweapons_caracara.meta"}});
    resources.front().SetManifest(std::move(manifest));

    const StreamingPlan plan = BuildPlan(resources);

    CHECK(plan.DataFiles().empty());
}

TEST_CASE("StreamingPlan: a DLC_ITYP_REQUEST matches a streamed .ytyp of any resource",
          "[streaming]")
{
    StreamTree tree;
    std::vector<Resource> resources;
    resources.push_back(tree.AddStreamResource("props"));
    tree.AddAsset("props", "sub/Shared_Props.ytyp", 2);

    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "dlc_ityp_request",
                                               .pattern = "stream/*.ytyp",
                                               .resolved = {"stream/shared_props.ytyp"}});
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("requester"), std::move(manifest)));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.DataFiles().size() == 1);
    const auto& dataFile = plan.DataFiles().front();
    CHECK(dataFile.type == "DLC_ITYP_REQUEST");
    CHECK(dataFile.matchesStreamedAsset);
    CHECK(dataFile.fileName == "shared_props.ytyp");
    CHECK_FALSE(dataFile.implicit);
}

TEST_CASE("StreamingPlan: a DLC_ITYP_REQUEST with no streamed .ytyp is kept but unmatched",
          "[streaming]")
{
    StreamTree tree;
    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{
        .type = "DLC_ITYP_REQUEST", .pattern = "vanilla.ytyp", .resolved = {"vanilla.ytyp"}});
    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("vanilla_ref"), std::move(manifest)));

    const StreamingPlan plan = BuildPlan(resources);

    REQUIRE(plan.DataFiles().size() == 1);
    CHECK_FALSE(plan.DataFiles().front().matchesStreamedAsset);
    CHECK(plan.DataFiles().front().fileName == "vanilla.ytyp");
}

TEST_CASE("StreamingPlan: a DLC_ITYP_REQUEST whose .ytyp is switched off is dropped", "[streaming]")
{
    StreamTree tree;
    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "DLC_ITYP_REQUEST",
                                               .pattern = "stream/props.ytyp",
                                               .resolved = {"stream/props.ytyp"}});
    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("props"), std::move(manifest)));
    tree.AddAsset("props", "props.ytyp", 2);

    StreamingSettings streaming;
    streaming.loadMaps = false;
    const StreamingPlan plan = BuildPlan(resources, streaming);

    CHECK(plan.DataFiles().empty());
}

TEST_CASE("StreamingPlan: auto_request_ytyp adds requests only where none exist", "[streaming]")
{
    StreamTree tree;
    ResourceManifest manifest;
    manifest.dataFiles.push_back(DataFileEntry{.type = "DLC_ITYP_REQUEST",
                                               .pattern = "stream/listed.ytyp",
                                               .resolved = {"stream/listed.ytyp"}});
    std::vector<Resource> resources;
    resources.push_back(
        spl::tests::WithManifest(tree.AddStreamResource("props"), std::move(manifest)));
    tree.AddAsset("props", "listed.ytyp", 2);
    tree.AddAsset("props", "forgotten.ytyp", 2);

    resources.push_back(tree.AddStreamResource("with_ymf"));
    tree.AddAsset("with_ymf", "types.ytyp", 2);
    tree.AddAsset("with_ymf", "_manifest.ymf", 2);

    SECTION("off by default")
    {
        const StreamingPlan plan = BuildPlan(resources);
        REQUIRE(plan.DataFiles().size() == 1);
        CHECK(plan.DataFiles().front().fileName == "listed.ytyp");
    }

    SECTION("on")
    {
        StreamingSettings streaming;
        streaming.autoRequestYtyp = true;
        const StreamingPlan plan = BuildPlan(resources, streaming);

        REQUIRE(plan.DataFiles().size() == 2);
        CHECK(plan.DataFiles()[0].fileName == "listed.ytyp");
        CHECK_FALSE(plan.DataFiles()[0].implicit);
        CHECK(plan.DataFiles()[1].fileName == "forgotten.ytyp");
        CHECK(plan.DataFiles()[1].implicit);
        CHECK(plan.DataFiles()[1].matchesStreamedAsset);
        CHECK(plan.DataFiles()[1].relativePath == "stream/forgotten.ytyp");
    }
}
