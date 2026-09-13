#include <algorithm>
#include <string>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "streaming/AssetType.h"

using spl::config::StreamingSettings;
using spl::streaming::AssetType;
using spl::streaming::AssetTypeInfo;
using spl::streaming::Classify;
using spl::streaming::ConfigGate;
using spl::streaming::GetAssetTypeInfo;
using spl::streaming::GetAssetTypes;
using spl::streaming::IsGateOpen;
using spl::streaming::RegistrationStage;
using spl::streaming::SupportTier;

TEST_CASE("AssetType: classifies every known extension", "[streaming]")
{
    REQUIRE(Classify("ydr") == AssetType::Drawable);
    REQUIRE(Classify("ytd") == AssetType::TextureDictionary);
    REQUIRE(Classify("ydd") == AssetType::DrawableDictionary);
    REQUIRE(Classify("yft") == AssetType::Fragment);
    REQUIRE(Classify("ymap") == AssetType::MapData);
    REQUIRE(Classify("ytyp") == AssetType::MapTypes);
    REQUIRE(Classify("ybn") == AssetType::StaticBounds);
    REQUIRE(Classify("ymf") == AssetType::PackfileManifest);
    REQUIRE(Classify("ymt") == AssetType::Metadata);
}

TEST_CASE("AssetType: classification ignores case and a leading dot", "[streaming]")
{
    REQUIRE(Classify("YDR") == AssetType::Drawable);
    REQUIRE(Classify(".Ytd") == AssetType::TextureDictionary);
    REQUIRE(Classify("YMAP") == AssetType::MapData);
}

TEST_CASE("AssetType: anything else is Unknown", "[streaming]")
{
    REQUIRE(Classify("") == AssetType::Unknown);
    REQUIRE(Classify(".") == AssetType::Unknown);
    REQUIRE(Classify("rpf") == AssetType::Unknown);
    REQUIRE(Classify("dll") == AssetType::Unknown);
    REQUIRE(Classify("ydrx") == AssetType::Unknown);
    REQUIRE(Classify("unknown") == AssetType::Unknown);
}

TEST_CASE("AssetType: the table is indexed by its own enumerator", "[streaming]")
{
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        REQUIRE(GetAssetTypeInfo(info.type).type == info.type);
    }
}

TEST_CASE("AssetType: every supported type but ymf has a streaming module", "[streaming]")
{
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        if (info.tier == SupportTier::Unsupported)
        {
            continue;
        }
        if (info.type == AssetType::PackfileManifest || info.type == AssetType::OtherModule)
        {
            REQUIRE(info.moduleExtension.empty()); // a manifest chunk, or the file's own store
            continue;
        }
        REQUIRE(info.moduleExtension == info.extension);
    }
}

TEST_CASE("AssetType: gfx is the only known type without an RSC header", "[streaming]")
{
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        if (info.type == AssetType::Unknown || info.type == AssetType::OtherModule)
        {
            continue;
        }
        REQUIRE(info.expectsRscHeader == (info.type != AssetType::Scaleform));
    }
}

TEST_CASE("AssetType: maps register after the types and collisions they use", "[streaming]")
{
    const AssetTypeInfo& types = GetAssetTypeInfo(AssetType::MapTypes);
    const AssetTypeInfo& bounds = GetAssetTypeInfo(AssetType::StaticBounds);
    const AssetTypeInfo& maps = GetAssetTypeInfo(AssetType::MapData);

    REQUIRE(types.stage == RegistrationStage::Late);
    REQUIRE(bounds.stage == RegistrationStage::Late);
    REQUIRE(maps.stage == RegistrationStage::Late);
    REQUIRE(types.registrationOrder < bounds.registrationOrder);
    REQUIRE(bounds.registrationOrder < maps.registrationOrder);

    REQUIRE(GetAssetTypeInfo(AssetType::TextureDictionary).stage == RegistrationStage::Early);
    REQUIRE(GetAssetTypeInfo(AssetType::Drawable).stage == RegistrationStage::Early);
}

TEST_CASE("AssetType: config gates match the [streaming] flags", "[streaming]")
{
    StreamingSettings settings;
    settings.loadTextures = false;
    settings.loadModels = false;
    settings.loadMaps = false;
    settings.loadCollisions = false;
    settings.loadManifests = false;

    REQUIRE(IsGateOpen(ConfigGate::Always, settings));
    REQUIRE_FALSE(IsGateOpen(ConfigGate::LoadTextures, settings));
    REQUIRE_FALSE(IsGateOpen(ConfigGate::LoadModels, settings));
    REQUIRE_FALSE(IsGateOpen(ConfigGate::LoadMaps, settings));
    REQUIRE_FALSE(IsGateOpen(ConfigGate::LoadCollisions, settings));
    REQUIRE_FALSE(IsGateOpen(ConfigGate::LoadManifests, settings));

    settings.loadMaps = true;
    REQUIRE(IsGateOpen(ConfigGate::LoadMaps, settings));

    REQUIRE(GetAssetTypeInfo(AssetType::TextureDictionary).gate == ConfigGate::LoadTextures);
    REQUIRE(GetAssetTypeInfo(AssetType::Fragment).gate == ConfigGate::LoadModels);
    REQUIRE(GetAssetTypeInfo(AssetType::MapTypes).gate == ConfigGate::LoadMaps);
    REQUIRE(GetAssetTypeInfo(AssetType::StaticBounds).gate == ConfigGate::LoadCollisions);
    REQUIRE(GetAssetTypeInfo(AssetType::PackfileManifest).gate == ConfigGate::LoadManifests);
}

TEST_CASE("AssetType: ToString gives the extension", "[streaming]")
{
    REQUIRE(spl::streaming::ToString(AssetType::MapData) == "ymap");
    REQUIRE(spl::streaming::ToString(AssetType::Unknown) == "unknown");
}

TEST_CASE("AssetType: every recognized type registers, and OtherModule uses the file's store",
          "[streaming]")
{
    for (const AssetTypeInfo& info : GetAssetTypes())
    {
        if (info.type != AssetType::Unknown)
        {
            CHECK(info.tier == SupportTier::Supported);
        }
    }
    REQUIRE(Classify("other") == AssetType::Unknown); // the row's label is not an extension
    REQUIRE(spl::streaming::GetModuleExtension(AssetType::OtherModule, "yld") == "yld");
    REQUIRE(spl::streaming::GetModuleExtension(AssetType::Drawable, "whatever") == "ydr");
    REQUIRE(GetAssetTypeInfo(AssetType::Navmesh).gate == ConfigGate::LoadMaps);
    REQUIRE(GetAssetTypeInfo(AssetType::PathNodes).gate == ConfigGate::LoadMaps);
}
