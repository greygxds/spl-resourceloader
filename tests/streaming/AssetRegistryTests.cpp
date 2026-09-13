#include <optional>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "streaming/AssetRegistry.h"
#include "streaming/AssetType.h"
#include "util/Hash.h"

using namespace spl;
using namespace spl::streaming;

namespace
{
RegisteredAsset MakeAsset(std::string fileName, AssetType type, uint32_t index)
{
    return RegisteredAsset{.owner = resource::ResourceId{0},
                           .resourceName = "map_one",
                           .vfsPath = "splres:/map_one/stream/" + fileName,
                           .fileName = std::move(fileName),
                           .moduleExtension = std::string{GetAssetTypeInfo(type).moduleExtension},
                           .type = type,
                           .globalIndex = rage::GlobalIndex{index},
                           .handle = rage::StreamingHandle{index}};
}
} // namespace

TEST_CASE("AssetRegistry: records assets in registration order", "[streaming]")
{
    AssetRegistry registry;
    registry.Add(MakeAsset("a.ytd", AssetType::TextureDictionary, 1));
    registry.Add(MakeAsset("b.ytd", AssetType::TextureDictionary, 2));

    REQUIRE(registry.Size() == 2);
    CHECK(registry.All()[0].fileName == "a.ytd");
    CHECK(registry.All()[1].fileName == "b.ytd");
}

TEST_CASE("AssetRegistry: finds an asset by file name", "[streaming]")
{
    AssetRegistry registry;
    registry.Add(MakeAsset("spl_test.ytd", AssetType::TextureDictionary, 7));

    const RegisteredAsset* found = registry.Find("spl_test.ytd");
    REQUIRE(found != nullptr);
    CHECK(found->globalIndex == rage::GlobalIndex{7});
    CHECK(found->vfsPath == "splres:/map_one/stream/spl_test.ytd");
    CHECK(registry.Find("missing.ytd") == nullptr);
}

TEST_CASE("AssetRegistry: counts and filters by type", "[streaming]")
{
    AssetRegistry registry;
    registry.Add(MakeAsset("a.ytd", AssetType::TextureDictionary, 1));
    registry.Add(MakeAsset("b.ydr", AssetType::Drawable, 2));
    registry.Add(MakeAsset("c.ytd", AssetType::TextureDictionary, 3));

    CHECK(registry.CountOf(AssetType::TextureDictionary) == 2);
    CHECK(registry.CountOf(AssetType::Drawable) == 1);
    CHECK(registry.CountOf(AssetType::MapData) == 0);

    const std::vector<RegisteredAsset> textures = registry.OfType(AssetType::TextureDictionary);
    REQUIRE(textures.size() == 2);
    CHECK(textures[0].fileName == "a.ytd");
    CHECK(textures[1].fileName == "c.ytd");
}

TEST_CASE("AssetRegistry: Clear forgets everything", "[streaming]")
{
    AssetRegistry registry;
    registry.Add(MakeAsset("a.ytd", AssetType::TextureDictionary, 1));
    registry.Clear();

    CHECK(registry.Size() == 0);
    CHECK(registry.Find("a.ytd") == nullptr);
}

TEST_CASE("AssetRegistry: finds the type request a manifest names by hash", "[streaming]")
{
    const std::vector<LoadedDataFile> dataFiles = {
        LoadedDataFile{.type = "DLC_ITYP_REQUEST",
                       .fileName = "spl_props.ytyp",
                       .entryName = "splres:/map_one/stream/spl_props.ytyp"},
        LoadedDataFile{.type = "DLC_ITYP_REQUEST",
                       .fileName = "other.ytyp",
                       .entryName = "splres:/map_one/stream/other.ytyp",
                       .released = true},
    };

    const LoadedDataFile* found = FindTypeRequest(dataFiles, util::JoaatLower("spl_props"));
    REQUIRE(found != nullptr);
    CHECK(found->fileName == "spl_props.ytyp");

    // A request that was already handed over is not handed over twice.
    CHECK(FindTypeRequest(dataFiles, util::JoaatLower("other")) == nullptr);
    CHECK(FindTypeRequest(dataFiles, util::JoaatLower("spl_props.ytyp")) == nullptr);
}

TEST_CASE("AssetRegistry: marks a data file released by the name the game was given", "[streaming]")
{
    AssetRegistry registry;
    registry.AddDataFile(LoadedDataFile{
        .type = "DLC_ITYP_REQUEST", .fileName = "props.ytyp", .entryName = "spl/props.ytyp"});

    CHECK_FALSE(registry.MarkDataFileReleased("props.ytyp"));
    CHECK(registry.MarkDataFileReleased("spl/props.ytyp"));
    CHECK(registry.DataFiles()[0].released);

    registry.Clear();
    CHECK(registry.DataFiles().empty());
}

TEST_CASE("AssetRegistry: a created slot has no game handle to go back to", "[streaming]")
{
    AssetRegistry registry;
    registry.PushHandle(rage::GlobalIndex{10}, rage::StreamingHandle{0x45});

    const HandleStack* stack = registry.FindHandleStack(rage::GlobalIndex{10});
    REQUIRE(stack != nullptr);
    CHECK_FALSE(stack->gameHandle.has_value());
    CHECK(stack->loaderHandles == std::vector{rage::StreamingHandle{0x45}});

    CHECK_FALSE(registry.PopHandle(rage::GlobalIndex{10}, rage::StreamingHandle{0x45}));
    CHECK(registry.FindHandleStack(rage::GlobalIndex{10}) == nullptr);
}

TEST_CASE("AssetRegistry: an override restores the game handle once it is gone", "[streaming]")
{
    const rage::GlobalIndex index{20};
    const rage::StreamingHandle game{(4u << 16) | 0x1234u};

    AssetRegistry registry;
    registry.PushHandle(index, rage::StreamingHandle{0x50}, game);

    CHECK(registry.PopHandle(index, rage::StreamingHandle{0x50}) == game);
    CHECK(registry.FindHandleStack(index) == nullptr);
}

TEST_CASE("AssetRegistry: stacked overrides keep the first game handle", "[streaming]")
{
    const rage::GlobalIndex index{30};
    const rage::StreamingHandle game{(4u << 16) | 7u};
    const rage::StreamingHandle first{0x60};
    const rage::StreamingHandle second{0x61};

    AssetRegistry registry;
    registry.PushHandle(index, first, game);
    // The second resource sees the first one's handle in the slot; that is not the game's.
    registry.PushHandle(index, second, first);

    const HandleStack* stack = registry.FindHandleStack(index);
    REQUIRE(stack != nullptr);
    CHECK(stack->gameHandle == game);
    CHECK(stack->loaderHandles == std::vector{first, second});

    // Removing the one underneath leaves the newest in place.
    CHECK(registry.PopHandle(index, first) == second);
    CHECK(registry.PopHandle(index, second) == game);
    CHECK_FALSE(registry.PopHandle(index, second).has_value());
}

TEST_CASE("AssetRegistry: counts overrides and forgets handle stacks on Clear", "[streaming]")
{
    AssetRegistry registry;
    RegisteredAsset overriding = MakeAsset("adder.yft", AssetType::Fragment, 5);
    overriding.overridesGameAsset = true;
    registry.Add(overriding);
    registry.Add(MakeAsset("new.ydr", AssetType::Drawable, 6));
    registry.PushHandle(rage::GlobalIndex{5}, rage::StreamingHandle{5}, rage::StreamingHandle{9});

    CHECK(registry.CountOverrides() == 1);
    registry.Clear();
    CHECK(registry.CountOverrides() == 0);
    CHECK(registry.FindHandleStack(rage::GlobalIndex{5}) == nullptr);
}
