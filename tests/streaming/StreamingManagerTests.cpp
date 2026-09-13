#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "manifest/ResourceManifest.h"
#include "streaming/AssetRegistry.h"
#include "streaming/AssetType.h"
#include "streaming/StreamingBackend.h"
#include "streaming/StreamingManager.h"
#include "streaming/StreamingPlan.h"
#include "tests/streaming/StreamTree.h"

using spl::config::DiagnosticsSettings;
using spl::config::StreamingSettings;
using namespace spl;
using namespace spl::streaming;

namespace
{
constexpr uint32_t kTextureVersion = 13;

/// A handle inside an archive, the kind a game asset has.
constexpr rage::StreamingHandle kGameHandle{(5u << 16) | 0x42u};

/// Records what the state machine asked for, and answers however the test wants it to.
class FakeStreamingBackend final : public IStreamingBackend
{
public:
    Result<void> PrepareResourceRoot(const std::filesystem::path& root) override
    {
        events.push_back("mount");
        ++mountCalls;
        mountedRoot = root;
        if (failMount)
        {
            return MakeError(ErrorCode::Unavailable, "the mount was refused");
        }
        return {};
    }

    RegistrationOutcome RegisterAsset(const PlannedAsset& asset) override
    {
        registered.push_back(asset.fileName);
        events.push_back("register " + asset.fileName);
        if (skip.contains(asset.fileName))
        {
            return RegistrationOutcome{.status = RegistrationStatus::Skipped,
                                       .message = "skipped '" + asset.fileName + "'"};
        }
        if (fail.contains(asset.fileName))
        {
            return RegistrationOutcome{.status = RegistrationStatus::Failed,
                                       .message = "failed '" + asset.fileName + "'"};
        }
        if (gameAssets.contains(asset.fileName) && keepGameAssets)
        {
            return RegistrationOutcome{.status = RegistrationStatus::KeptGameAsset,
                                       .message = "kept '" + asset.fileName + "'"};
        }
        const auto index = static_cast<uint32_t>(registered.size());
        RegistrationOutcome outcome{.status = RegistrationStatus::Registered,
                                    .vfsPath = "splres:/" + asset.resourceName + "/stream/" +
                                               asset.fileName,
                                    .globalIndex = rage::GlobalIndex{index},
                                    .handle = rage::StreamingHandle{index}};
        if (gameAssets.contains(asset.fileName))
        {
            outcome.replacedHandle = kGameHandle;
            outcome.overrideTiming = inUse.contains(asset.fileName) ? OverrideTiming::AfterUnload
                                                                    : OverrideTiming::NextLoad;
        }
        return outcome;
    }

    Result<void> InstallMapTypesPatches() override
    {
        events.push_back("patches");
        if (failPatches)
        {
            return MakeError(ErrorCode::NotFound, "the patch was refused");
        }
        return {};
    }

    Result<void> InstallMapDataPatches() override
    {
        events.push_back("map patches");
        return {};
    }

    Result<std::string> LoadDataFile(const PlannedDataFile& dataFile) override
    {
        events.push_back("data file " + dataFile.fileName);
        if (fail.contains(dataFile.fileName + " data file"))
        {
            return MakeError(ErrorCode::Unavailable, "the mounter refused it");
        }
        return "splres:/" + dataFile.resourceName + "/" + dataFile.relativePath;
    }

    Result<PackfileManifestOutcome>
    LoadPackfileManifest(const PlannedManifest& manifest,
                         std::span<const LoadedDataFile> loadedDataFiles) override
    {
        events.push_back("manifest " + manifest.fileName);
        if (fail.contains(manifest.fileName))
        {
            return MakeError(ErrorCode::Unavailable, "the parser refused it");
        }

        PackfileManifestOutcome outcome;
        for (const LoadedDataFile& dataFile : loadedDataFiles)
        {
            if (manifestTakesOver.contains(dataFile.fileName))
            {
                outcome.releasedTypeRequests.push_back(dataFile.entryName);
            }
        }
        outcome.mapDependencies.push_back(
            MapDependency{.owner = manifest.owner, .mapDataHash = 1, .mapTypesHash = 2});
        return outcome;
    }

    bool CanReloadMapStore() override
    {
        return reloadAllowed;
    }

    Result<MapReloadReport> ReloadMapStore(std::span<const RegisteredAsset> collisions) override
    {
        events.push_back("reload");
        for (const RegisteredAsset& collision : collisions)
        {
            reloadedCollisions.push_back(collision.fileName);
        }
        if (failReload)
        {
            return MakeError(ErrorCode::Unavailable, "the rebuild faulted");
        }
        return MapReloadReport{.method = "fake", .collisionsPreloaded = collisions.size()};
    }

    void FinishDataFiles(const DataFileFinishRequest& request) override
    {
        events.push_back("finish data files");
        finishRequests.push_back(request);
    }

    std::optional<ReassertOutcome> ReassertAsset(const RegisteredAsset& asset) override
    {
        if (displacedByGame.erase(asset.fileName) == 0)
        {
            return std::nullopt;
        }
        return ReassertOutcome{.displaced = kGameHandle};
    }

    Result<void> UnregisterAsset(const RegisteredAsset&) override
    {
        return MakeError(ErrorCode::NotSupported, "not implemented");
    }

    std::vector<std::string> registered;
    std::vector<std::string> events; ///< every call, in order
    std::set<std::string> skip;
    std::set<std::string> fail;
    std::set<std::string> manifestTakesOver; ///< .ytyp file names a manifest releases
    std::set<std::string> gameAssets;        ///< names the game already has
    std::set<std::string> inUse;             ///< game assets whose copy is referenced
    std::set<std::string> displacedByGame;   ///< slots the game re-registered after us, once
    bool keepGameAssets = false;             ///< what allow_overrides = false does
    std::vector<std::string> reloadedCollisions;
    std::vector<DataFileFinishRequest> finishRequests;
    std::filesystem::path mountedRoot;
    int mountCalls = 0;
    bool failMount = false;
    bool failPatches = false;
    bool failReload = false;
    bool reloadAllowed = true;
};

/// A manifest with one DLC_ITYP_REQUEST per path, the way FiveM prop resources write it.
manifest::ResourceManifest WithTypeRequests(std::initializer_list<std::string> relativePaths)
{
    manifest::ResourceManifest requests;
    for (const std::string& relativePath : relativePaths)
    {
        requests.dataFiles.push_back(manifest::DataFileEntry{
            .type = "DLC_ITYP_REQUEST", .pattern = relativePath, .resolved = {relativePath}});
    }
    return requests;
}

/// One resource called map_one, holding the given stream files.
struct Fixture
{
    tests::StreamTree tree;
    std::vector<resource::Resource> resources;
    StreamingPlan plan;

    void Build(std::span<const std::string_view> fileNames,
               manifest::ResourceManifest resourceManifest = {},
               const StreamingSettings& settings = StreamingSettings{})
    {
        resources.push_back(
            tests::WithManifest(tree.AddStreamResource("map_one"), std::move(resourceManifest)));
        for (std::string_view fileName : fileNames)
        {
            tree.AddAsset("map_one", fileName, kTextureVersion);
        }
        plan = StreamingPlan::Build(resources, settings, DiagnosticsSettings{});
    }
};

/// Runs the pump to completion, so a test that does not care about batching does not have to
/// count ticks. The bound is generous but finite: a stuck stage must fail the test, not hang.
void RunToCompletion(StreamingManager& manager)
{
    for (int tick = 0; tick < 100 && !manager.IsFinished(); ++tick)
    {
        manager.Tick();
    }
}
} // namespace

TEST_CASE("StreamingManager: mounts before it registers anything", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());

    CHECK(manager.GetStage() == StreamingStage::Mount);
    manager.Tick();
    CHECK(backend.mountCalls == 1);
    CHECK(backend.mountedRoot == fixture.tree.Path());
    CHECK(backend.registered.empty());
    CHECK(manager.GetStage() == StreamingStage::RegisterEarly);
}

TEST_CASE("StreamingManager: a failed mount stops the pump", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.failMount = true;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Failed);
    CHECK(backend.registered.empty());
    CHECK(manager.GetRegistry().Size() == 0);
}

TEST_CASE("StreamingManager: registers every planned asset and reaches Done", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytd"},
                              std::string_view{"sub/c.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(backend.registered.size() == 3);
    CHECK(manager.GetRegistry().Size() == 3);
    CHECK(manager.GetTotals().registered == 3);
    CHECK(manager.GetTotals().skipped == 0);
    CHECK(manager.GetTotals().failed == 0);

    const RegisteredAsset* found = manager.GetRegistry().Find("c.ytd");
    REQUIRE(found != nullptr);
    CHECK(found->moduleExtension == "ytd");
    CHECK(found->type == AssetType::TextureDictionary);
    CHECK(found->vfsPath == "splres:/map_one/stream/c.ytd");
}

TEST_CASE("StreamingManager: a tick registers at most the batch size", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytd"},
                              std::string_view{"c.ytd"}, std::string_view{"d.ytd"},
                              std::string_view{"e.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{.maxRegistrationsPerTick = 2});

    manager.Tick(); // the mount
    CHECK(backend.registered.empty());
    manager.Tick();
    CHECK(backend.registered.size() == 2);
    CHECK(manager.GetStage() == StreamingStage::RegisterEarly);
    manager.Tick();
    CHECK(backend.registered.size() == 4);
    manager.Tick();
    CHECK(backend.registered.size() == 5);

    RunToCompletion(manager);
    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetRegistry().Size() == 5);
}

TEST_CASE("StreamingManager: one failure does not stop the others", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytd"},
                              std::string_view{"c.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.fail.insert("b.ytd");
    backend.skip.insert("c.ytd");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(backend.registered.size() == 3); // all three were attempted
    CHECK(manager.GetTotals().registered == 1);
    CHECK(manager.GetTotals().failed == 1);
    CHECK(manager.GetTotals().skipped == 1);

    // Only what the game accepted is recorded, because only that has to be undone later.
    CHECK(manager.GetRegistry().Size() == 1);
    CHECK(manager.GetRegistry().Find("a.ytd") != nullptr);
    CHECK(manager.GetRegistry().Find("b.ytd") == nullptr);
    CHECK(manager.GetRegistry().Find("c.ytd") == nullptr);
}

TEST_CASE("StreamingManager: ticking after Done changes nothing", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);
    REQUIRE(manager.GetStage() == StreamingStage::Done);

    manager.Tick();
    manager.Tick();
    CHECK(backend.registered.size() == 1);
    CHECK(backend.mountCalls == 1);
}

TEST_CASE("StreamingManager: every supported type is registered, except manifests", "[streaming]")
{
    // The type gate still exists for unsupported types; today it only keeps .ymf out.
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        const bool expected =
            info.tier == SupportTier::Supported && info.type != AssetType::PackfileManifest;
        CHECK(IsRegistrationImplemented(info.type) == expected);
    }
}

TEST_CASE("StreamingManager: types, collisions and maps register in that order after both patch "
          "sets",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"area.ymap"}, std::string_view{"ground.ybn"},
                              std::string_view{"props.ytyp"}, std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(backend.events == std::vector<std::string>{"mount", "register a.ytd", "patches",
                                                     "map patches", "register props.ytyp",
                                                     "register ground.ybn", "register area.ymap",
                                                     "reload"});
    CHECK(manager.GetTotals().registered == 4);
    CHECK(manager.GetTotals().deferred == 0);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: collisions alone need the map data patches but not the types ones",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"ground.ybn"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::find(backend.events, "patches") == backend.events.end());
    CHECK(std::ranges::find(backend.events, "map patches") != backend.events.end());
}

TEST_CASE("StreamingManager: the map store is reloaded once with every registered collision",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ybn"}, std::string_view{"b.ybn"},
                              std::string_view{"bad.ybn"}, std::string_view{"area.ymap"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.fail.insert("bad.ybn");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::count(backend.events, "reload") == 1);
    // Only what the game accepted is preloaded.
    CHECK(backend.reloadedCollisions == std::vector<std::string>{"a.ybn", "b.ybn"});
    CHECK(manager.GetMapReloadResult() == MapReloadResult::Reloaded);
}

TEST_CASE("StreamingManager: no maps, collisions or this_is_a_map means no reload", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"prop.ydr"}, std::string_view{"props.ytyp"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::find(backend.events, "reload") == backend.events.end());
    CHECK(manager.GetMapReloadResult() == MapReloadResult::NotNeeded);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: this_is_a_map reloads the map store even without map files",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"prop.ydr"}};
    manifest::ResourceManifest isMap;
    isMap.isMap = true;
    fixture.Build(names, std::move(isMap));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::count(backend.events, "reload") == 1);
    CHECK(backend.reloadedCollisions.empty());
}

TEST_CASE("StreamingManager: the reload waits while the backend says it is not safe", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"area.ymap"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.reloadAllowed = false;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::MapReload);
    CHECK_FALSE(manager.IsFinished());
    CHECK(std::ranges::find(backend.events, "reload") == backend.events.end());

    backend.reloadAllowed = true;
    manager.Tick();
    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(std::ranges::count(backend.events, "reload") == 1);
}

TEST_CASE("StreamingManager: a failed reload still finishes the pump", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"area.ymap"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.failReload = true;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetMapReloadResult() == MapReloadResult::Failed);
    CHECK(manager.GetRegistry().Find("area.ymap") != nullptr);
}

TEST_CASE("StreamingManager: manifests load after data files and before the reload", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"props.ytyp"}, std::string_view{"area.ymap"},
                              std::string_view{"map_one.ymf"}};
    fixture.Build(names, WithTypeRequests({"stream/props.ytyp"}));

    FakeStreamingBackend backend;
    backend.manifestTakesOver.insert("props.ytyp");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    const auto position = [&](std::string_view event)
    { return std::ranges::find(backend.events, event) - backend.events.begin(); };
    CHECK(position("data file props.ytyp") < position("manifest map_one.ymf"));
    CHECK(position("manifest map_one.ymf") < position("reload"));
    CHECK(position("reload") < static_cast<std::ptrdiff_t>(backend.events.size()));
    CHECK(std::ranges::find(backend.registered, "map_one.ymf") == backend.registered.end());

    CHECK(manager.GetManifestTotals().loaded == 1);
    CHECK(manager.GetManifestTotals().releasedTypeRequests == 1);
    REQUIRE(manager.GetRegistry().DataFiles().size() == 1);
    CHECK(manager.GetRegistry().DataFiles()[0].released);
    CHECK(manager.GetRegistry().MapDependencies().size() == 1);
}

TEST_CASE("StreamingManager: one failed manifest does not stop the reload", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"area.ymap"}, std::string_view{"map_one.ymf"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.fail.insert("map_one.ymf");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetManifestTotals().failed == 1);
    CHECK(std::ranges::count(backend.events, "reload") == 1);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: loaded data files are recorded with the name the game got",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"props.ytyp"}};
    fixture.Build(names, WithTypeRequests({"stream/props.ytyp"}));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    REQUIRE(manager.GetRegistry().DataFiles().size() == 1);
    const LoadedDataFile& loaded = manager.GetRegistry().DataFiles()[0];
    CHECK(loaded.type == "DLC_ITYP_REQUEST");
    CHECK(loaded.fileName == "props.ytyp");
    CHECK(loaded.entryName == "splres:/map_one/stream/props.ytyp");
    CHECK_FALSE(loaded.released);
}

TEST_CASE("StreamingManager: models and archetypes are registered", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"prop.ydr"}, std::string_view{"set.ydd"},
                              std::string_view{"door.yft"}, std::string_view{"props.ytyp"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetTotals().registered == 4);
    CHECK(manager.GetTotals().deferred == 0);
    REQUIRE(manager.GetRegistry().Find("props.ytyp") != nullptr);
    CHECK(manager.GetRegistry().Find("props.ytyp")->type == AssetType::MapTypes);
}

TEST_CASE("StreamingManager: early assets, then patches, then types, then data files",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"props.ytyp"}, std::string_view{"prop.ydr"},
                              std::string_view{"prop.ytd"}};
    fixture.Build(names, WithTypeRequests({"stream/props.ytyp"}));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(backend.events ==
          std::vector<std::string>{"mount", "register prop.ytd", "register prop.ydr", "patches",
                                   "register props.ytyp", "data file props.ytyp"});
    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetDataFileTotals().loaded == 1);
}

TEST_CASE("StreamingManager: no types means no game patches", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"prop.ydr"}, std::string_view{"prop.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::find(backend.events, "patches") == backend.events.end());
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: a refused patch does not stop the types from registering",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"props.ytyp"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.failPatches = true;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetRegistry().Find("props.ytyp") != nullptr);
}

TEST_CASE("StreamingManager: a request whose .ytyp was not registered is not loaded", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"good.ytyp"}, std::string_view{"bad.ytyp"}};
    fixture.Build(names, WithTypeRequests({"stream/good.ytyp", "stream/bad.ytyp"}));

    FakeStreamingBackend backend;
    backend.skip.insert("bad.ytyp");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::find(backend.events, "data file good.ytyp") != backend.events.end());
    CHECK(std::ranges::find(backend.events, "data file bad.ytyp") == backend.events.end());
    CHECK(manager.GetDataFileTotals().loaded == 1);
    CHECK(manager.GetDataFileTotals().skipped == 1);
}

TEST_CASE("StreamingManager: one failed data file does not stop the others", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytyp"}, std::string_view{"b.ytyp"}};
    fixture.Build(names, WithTypeRequests({"stream/a.ytyp", "stream/b.ytyp"}));

    FakeStreamingBackend backend;
    backend.fail.insert("a.ytyp data file");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetDataFileTotals().failed == 1);
    CHECK(manager.GetDataFileTotals().loaded == 1);
}

TEST_CASE("StreamingManager: a tick loads at most the data-file batch size", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytyp"}, std::string_view{"b.ytyp"},
                              std::string_view{"c.ytyp"}};
    fixture.Build(names, WithTypeRequests({"stream/a.ytyp", "stream/b.ytyp", "stream/c.ytyp"}));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{.maxDataFilesPerTick = 2});

    while (manager.GetStage() != StreamingStage::DataFiles)
    {
        manager.Tick();
    }
    manager.Tick();
    CHECK(manager.GetDataFileTotals().loaded == 2);
    CHECK(manager.GetStage() == StreamingStage::DataFiles);
    manager.Tick();
    CHECK(manager.GetDataFileTotals().loaded == 3);

    RunToCompletion(manager);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: auto_request_ytyp loads a .ytyp the manifest forgot", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"props.ytyp"}};
    StreamingSettings settings;
    settings.autoRequestYtyp = true;
    fixture.Build(names, {}, settings);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(backend.events.back() == "data file props.ytyp");
    CHECK(manager.GetDataFileTotals().loaded == 1);
}

TEST_CASE("StreamingManager: an override is registered with the game handle underneath",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"commonmenu.ytd"}, std::string_view{"new.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.gameAssets.insert("commonmenu.ytd");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetTotals().registered == 2);
    const RegisteredAsset* overriding = manager.GetRegistry().Find("commonmenu.ytd");
    REQUIRE(overriding != nullptr);
    CHECK(overriding->overridesGameAsset);
    CHECK_FALSE(manager.GetRegistry().Find("new.ytd")->overridesGameAsset);
    CHECK(manager.GetRegistry().CountOverrides() == 1);

    const HandleStack* stack = manager.GetRegistry().FindHandleStack(overriding->globalIndex);
    REQUIRE(stack != nullptr);
    CHECK(stack->gameHandle == kGameHandle);
    CHECK(stack->loaderHandles == std::vector{overriding->handle});

    const HandleStack* created =
        manager.GetRegistry().FindHandleStack(manager.GetRegistry().Find("new.ytd")->globalIndex);
    REQUIRE(created != nullptr);
    CHECK_FALSE(created->gameHandle.has_value());
}

TEST_CASE("StreamingManager: an override still in use by the game is registered all the same",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"adder.yft"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.gameAssets.insert("adder.yft");
    backend.inUse.insert("adder.yft");
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetTotals().registered == 1);
    REQUIRE(manager.GetRegistry().Find("adder.yft") != nullptr);
    CHECK(manager.GetRegistry().Find("adder.yft")->overridesGameAsset);
}

TEST_CASE("StreamingManager: a kept game asset counts as skipped and blocks its type request",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"v_int_1.ytyp"}, std::string_view{"new.ytd"}};
    fixture.Build(names, WithTypeRequests({"stream/v_int_1.ytyp"}));

    FakeStreamingBackend backend;
    backend.gameAssets.insert("v_int_1.ytyp");
    backend.keepGameAssets = true;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.GetTotals().registered == 1);
    CHECK(manager.GetTotals().skipped == 1);
    CHECK(manager.GetTotals().failed == 0);
    CHECK(manager.GetRegistry().CountOverrides() == 0);
    // Loading the request would load the game's own file under our resource's name.
    CHECK(std::ranges::find(backend.events, "data file v_int_1.ytyp") == backend.events.end());
    CHECK(manager.GetDataFileTotals().skipped == 1);
}

TEST_CASE("StreamingManager: a tick stops starting registrations once its budget is spent",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytd"},
                              std::string_view{"c.ytd"}, std::string_view{"d.ytd"},
                              std::string_view{"e.ytd"}};
    fixture.Build(names);

    // Every registration "takes" 3 ms, so a 5 ms budget fits two per tick.
    FakeStreamingBackend backend;
    int64_t nowMicros = 0;
    StreamingManager manager;
    manager.Start(
        fixture.plan, backend, fixture.tree.Path(),
        StreamingManager::Options{
            .tickBudgetMicros = 5000,
            .clockMicros = [&]
            { return nowMicros + 3000 * static_cast<int64_t>(backend.registered.size()); }});

    manager.Tick(); // the mount
    manager.Tick();
    CHECK(backend.registered.size() == 2);
    manager.Tick();
    CHECK(backend.registered.size() == 4);

    RunToCompletion(manager);
    CHECK(manager.GetRegistry().Size() == 5);
}

TEST_CASE("StreamingManager: a slow first registration still makes progress", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{
                      .tickBudgetMicros = 1,
                      .clockMicros = [&]
                      { return 1'000'000 * static_cast<int64_t>(backend.registered.size()); }});

    manager.Tick(); // the mount
    manager.Tick();
    CHECK(backend.registered.size() == 1);
    manager.Tick();
    CHECK(backend.registered.size() == 2);
}

TEST_CASE("StreamingManager: busy while changing game state, idle while waiting and after",
          "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"area.ymap"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.reloadAllowed = false;
    std::vector<bool> changes;
    StreamingManager manager;
    manager.Start(
        fixture.plan, backend, fixture.tree.Path(),
        StreamingManager::Options{.onBusyChanged = [&](bool busy) { changes.push_back(busy); }});

    RunToCompletion(manager);
    CHECK(manager.GetStage() == StreamingStage::MapReload);
    CHECK(changes == std::vector<bool>{true, false});

    backend.reloadAllowed = true;
    manager.Tick();
    CHECK(manager.GetStage() == StreamingStage::Done);
    CHECK(changes == std::vector<bool>{true, false, true, false});
}

TEST_CASE("StreamingManager: a failed mount leaves the pump idle", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    backend.failMount = true;
    bool busy = false;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{.onBusyChanged = [&](bool value) { busy = value; }});
    manager.Tick();

    CHECK(manager.GetStage() == StreamingStage::Failed);
    CHECK_FALSE(busy);
}

TEST_CASE("StreamingManager: stage timings list each stage that ran, in order", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}, std::string_view{"b.ytyp"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{.maxRegistrationsPerTick = 1});
    RunToCompletion(manager);

    std::vector<StreamingStage> stages;
    for (const StageTiming& timing : manager.GetStageTimings())
    {
        stages.push_back(timing.stage);
        CHECK(timing.ticks >= 1);
    }
    CHECK(stages.front() == StreamingStage::Mount);
    CHECK(std::ranges::find(stages, StreamingStage::RegisterEarly) != stages.end());
    CHECK(std::ranges::find(stages, StreamingStage::RegisterLate) != stages.end());
    CHECK(manager.GetCurrentWork().stage == StreamingStage::Done);
}

TEST_CASE("StreamingManager: GTXD_PARENTING_DATA loads after the map store reload", "[streaming]")
{
    Fixture fixture;
    manifest::ResourceManifest dataFiles;
    dataFiles.dataFiles.push_back(manifest::DataFileEntry{
        .type = "GTXD_PARENTING_DATA", .pattern = "gtxd.meta", .resolved = {"gtxd.meta"}});
    dataFiles.dataFiles.push_back(manifest::DataFileEntry{
        .type = "HANDLING_FILE", .pattern = "handling.meta", .resolved = {"handling.meta"}});
    const std::array names = {std::string_view{"area.ymap"}};
    fixture.Build(names, std::move(dataFiles));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    const auto position = [&](std::string_view event)
    { return std::ranges::find(backend.events, event) - backend.events.begin(); };
    CHECK(position("data file handling.meta") < position("reload"));
    CHECK(position("reload") < position("data file gtxd.meta"));
    CHECK(position("data file gtxd.meta") < static_cast<std::ptrdiff_t>(backend.events.size()));
    CHECK(manager.GetDataFileTotals().loaded == 2);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: GTXD_PARENTING_DATA still loads when no reload is needed",
          "[streaming]")
{
    Fixture fixture;
    manifest::ResourceManifest dataFiles;
    dataFiles.dataFiles.push_back(manifest::DataFileEntry{
        .type = "GTXD_PARENTING_DATA", .pattern = "gtxd.meta", .resolved = {"gtxd.meta"}});
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names, std::move(dataFiles));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(std::ranges::count(backend.events, "reload") == 0);
    CHECK(std::ranges::count(backend.events, "data file gtxd.meta") == 1);
    CHECK(manager.GetStage() == StreamingStage::Done);
}

TEST_CASE("StreamingManager: paint ramps and ped folders are requested after the data files",
          "[streaming]")
{
    Fixture fixture;
    manifest::ResourceManifest dataFiles;
    dataFiles.dataFiles.push_back(manifest::DataFileEntry{
        .type = "CARCOLS_FILE", .pattern = "carcols.meta", .resolved = {"carcols.meta"}});
    const std::array names = {std::string_view{"myped^head_000_r.ydd"},
                              std::string_view{"myped^uppr_000_u.ydd"},
                              std::string_view{"car.yft"}};
    fixture.Build(names, std::move(dataFiles));

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    REQUIRE(backend.finishRequests.size() == 1);
    CHECK(backend.finishRequests[0].vehicleColoursLoaded);
    CHECK(backend.finishRequests[0].pedFolders == std::vector<std::string>{"myped"});
    const auto position = [&](std::string_view event)
    { return std::ranges::find(backend.events, event) - backend.events.begin(); };
    CHECK(position("data file carcols.meta") < position("finish data files"));
}

TEST_CASE("StreamingManager: nothing to finish means no finish call", "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"a.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path());
    RunToCompletion(manager);

    CHECK(backend.finishRequests.empty());
}

TEST_CASE(
    "StreamingManager: registrations the game replaced are put back once registration is done",
    "[streaming]")
{
    Fixture fixture;
    const std::array names = {std::string_view{"skin.ytd"}, std::string_view{"new.ytd"}};
    fixture.Build(names);

    FakeStreamingBackend backend;
    StreamingManager manager;
    manager.Start(fixture.plan, backend, fixture.tree.Path(),
                  StreamingManager::Options{.maxRegistrationsPerTick = 1});
    manager.Tick(); // mount
    backend.displacedByGame.insert("new.ytd");
    CHECK(manager.ReassertRegistrations() == 0); // not done yet: nothing is checked

    RunToCompletion(manager);
    REQUIRE(manager.GetStage() == StreamingStage::Done);
    CHECK(manager.ReassertRegistrations() == 1);
    CHECK(manager.ReassertRegistrations() == 0); // the fake only displaces once

    const RegisteredAsset* const reasserted = manager.GetRegistry().Find("new.ytd");
    REQUIRE(reasserted != nullptr);
    CHECK(reasserted->overridesGameAsset);
    const HandleStack* const stack = manager.GetRegistry().FindHandleStack(reasserted->globalIndex);
    REQUIRE(stack != nullptr);
    CHECK(stack->gameHandle == kGameHandle);
    CHECK_FALSE(manager.GetRegistry().Find("skin.ytd")->overridesGameAsset);
}
