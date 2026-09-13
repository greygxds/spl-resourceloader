#include <algorithm>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "resource/Resource.h"
#include "resource/ResourceManager.h"
#include "tests/TempTree.h"

using spl::config::LoaderConfig;
using spl::resource::Resource;
using spl::resource::ResourceManager;
using spl::resource::ResourceState;
using spl::tests::TempTree;

namespace
{
std::vector<std::string> LoadOrder(const ResourceManager& manager)
{
    std::vector<std::string> names;
    for (const Resource& resource : manager.GetResources())
    {
        names.push_back(resource.GetName());
    }
    return names;
}
} // namespace

TEST_CASE("ResourceManager: sorts unlisted resources by name", "[resource]")
{
    TempTree tree;
    tree.AddResource("charlie");
    tree.AddResource("alpha");
    tree.AddResource("Bravo");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());

    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"alpha", "Bravo", "charlie"});
    REQUIRE(manager.CountEnabled() == 3);
}

TEST_CASE("ResourceManager: priority entries load first, in the listed order", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.AddResource("bravo");
    tree.AddResource("charlie");

    LoaderConfig config;
    config.resources.priority = {"charlie", "bravo"};

    ResourceManager manager;
    manager.Discover(config, tree.Root());

    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"charlie", "bravo", "alpha"});
}

TEST_CASE("ResourceManager: priority matching ignores case", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.AddResource("Bravo");

    LoaderConfig config;
    config.resources.priority = {"BRAVO"};

    ResourceManager manager;
    manager.Discover(config, tree.Root());

    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"Bravo", "alpha"});
}

TEST_CASE("ResourceManager: disabled resources are kept but not enabled", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.AddResource("bravo");

    LoaderConfig config;
    config.resources.disabled = {"ALPHA"}; // case-insensitive

    ResourceManager manager;
    manager.Discover(config, tree.Root());

    REQUIRE(manager.GetResources().size() == 2);
    REQUIRE(manager.CountEnabled() == 1);

    const Resource* alpha = manager.Find("alpha");
    REQUIRE(alpha != nullptr);
    REQUIRE(alpha->GetState() == ResourceState::Disabled);
    REQUIRE_FALSE(alpha->IsEnabled());
    REQUIRE(alpha->GetStateReason() == "disabled by configuration");

    REQUIRE(manager.Find("bravo")->IsEnabled());
}

TEST_CASE("ResourceManager: auto_discover off loads only the priority list", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.AddResource("bravo");
    tree.AddResource("charlie");

    LoaderConfig config;
    config.resources.autoDiscover = false;
    config.resources.priority = {"charlie"};

    ResourceManager manager;
    manager.Discover(config, tree.Root());

    REQUIRE(manager.GetResources().size() == 3);
    REQUIRE(manager.CountEnabled() == 1);
    REQUIRE(manager.Find("charlie")->IsEnabled());
    REQUIRE(manager.Find("alpha")->GetStateReason() == "not in priority list");
}

TEST_CASE("ResourceManager: Find is case-insensitive and misses cleanly", "[resource]")
{
    TempTree tree;
    tree.AddResource("Map_One");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());

    REQUIRE(manager.Find("map_one") != nullptr);
    REQUIRE(manager.Find("MAP_ONE") != nullptr);
    REQUIRE(manager.Find("absent") == nullptr);
}

TEST_CASE("ResourceManager: a missing root is created and discovers nothing", "[resource]")
{
    TempTree tree;
    const std::filesystem::path root = tree.Root() / "resources";

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, root);

    REQUIRE(std::filesystem::is_directory(root));
    REQUIRE(manager.GetResources().empty());
    REQUIRE(manager.CountEnabled() == 0);
}

TEST_CASE("ResourceManager: a resource knows about its stream folder", "[resource]")
{
    TempTree tree;
    tree.AddResource("with_stream");
    tree.AddDirectory("with_stream/stream");
    tree.AddResource("without_stream");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());

    const Resource* withStream = manager.Find("with_stream");
    REQUIRE(withStream != nullptr);
    REQUIRE(withStream->HasStreamDirectory());
    REQUIRE(withStream->GetStreamPath() == withStream->GetRootPath() / "stream");

    REQUIRE_FALSE(manager.Find("without_stream")->HasStreamDirectory());
}

TEST_CASE("ResourceManager: discovery replaces the previous result", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());
    REQUIRE(manager.GetResources().size() == 1);

    tree.AddResource("bravo");
    manager.Discover(LoaderConfig{}, tree.Root());

    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"alpha", "bravo"});
}

TEST_CASE("ResourceManager: a quarantined resource is disabled, whatever its case", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.AddResource("Bad_Map");

    const std::vector<std::string> quarantined{"bad_map"};
    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root(), quarantined);

    const spl::resource::Resource* bad = manager.Find("bad_map");
    REQUIRE(bad != nullptr);
    CHECK(bad->GetState() == spl::resource::ResourceState::Disabled);
    CHECK(bad->GetStateReason() == "quarantined after a crash");
    CHECK(manager.CountEnabled() == 1);
}

TEST_CASE("ResourceManager: adopted mods sort after resources with manifests loaded", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.WriteFile("cache/mymod/fxmanifest.lua", "game 'gta5'\n");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());

    spl::resource::ResourceCandidate mod{.name = "mymod",
                                         .root = tree.Path() / "cache" / "mymod",
                                         .manifestPath =
                                             tree.Path() / "cache" / "mymod" / "fxmanifest.lua",
                                         .manifestKind = spl::resource::ManifestKind::FxManifest,
                                         .isMod = true};
    const std::vector<std::string> kept = manager.Adopt(LoaderConfig{}, {std::move(mod)});

    REQUIRE(kept == std::vector<std::string>{"mymod"});
    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"alpha", "mymod"});
    const spl::resource::Resource* found = manager.Find("mymod");
    REQUIRE(found != nullptr);
    CHECK(found->IsEnabled());
    CHECK(found->GetManifest() != nullptr);
}

TEST_CASE("ResourceManager: an adopted name that is taken is skipped", "[resource]")
{
    TempTree tree;
    tree.AddResource("alpha");
    tree.WriteFile("cache/alpha/fxmanifest.lua", "game 'gta5'\n");

    ResourceManager manager;
    manager.Discover(LoaderConfig{}, tree.Root());

    spl::resource::ResourceCandidate mod{.name = "Alpha",
                                         .root = tree.Path() / "cache" / "alpha",
                                         .manifestPath =
                                             tree.Path() / "cache" / "alpha" / "fxmanifest.lua",
                                         .manifestKind = spl::resource::ManifestKind::FxManifest,
                                         .isMod = true};
    const std::vector<std::string> kept = manager.Adopt(LoaderConfig{}, {std::move(mod)});

    CHECK(kept.empty());
    REQUIRE(LoadOrder(manager) == std::vector<std::string>{"alpha"});
}

TEST_CASE("ResourceManager: resource disabled and auto_discover settings leave mods alone",
          "[resource]")
{
    TempTree tree;
    tree.WriteFile("cache/mymod/fxmanifest.lua", "game 'gta5'\n");

    LoaderConfig config;
    config.resources.disabled = {"mymod"};
    config.resources.autoDiscover = false;

    ResourceManager manager;
    manager.Discover(config, tree.Root());

    spl::resource::ResourceCandidate mod{.name = "mymod",
                                         .root = tree.Path() / "cache" / "mymod",
                                         .manifestPath =
                                             tree.Path() / "cache" / "mymod" / "fxmanifest.lua",
                                         .manifestKind = spl::resource::ManifestKind::FxManifest,
                                         .isMod = true};
    const std::vector<std::string> kept = manager.Adopt(config, {std::move(mod)});

    REQUIRE(kept == std::vector<std::string>{"mymod"});
    CHECK(manager.Find("mymod")->IsEnabled());
}
