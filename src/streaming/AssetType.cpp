#include "streaming/AssetType.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "config/LoaderConfig.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
/// THE asset type table. Registration order within a stage follows the row order, which is
/// why ytyp, ybn and ymap appear in that sequence: a map refers to types and collisions that
/// have to exist first (FiveM's LoadType deferral).
///
/// knownRscVersion values are the versions real files carry; they are informational, so an
/// unexpected one only produces a warning.
constexpr std::array<AssetTypeInfo, kAssetTypeCount> kAssetTypes{{
    {.type = AssetType::TextureDictionary,
     .extension = "ytd",
     .moduleExtension = "ytd",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadTextures,
     .registrationOrder = 0,
     .knownRscVersion = 13},
    {.type = AssetType::Drawable,
     .extension = "ydr",
     .moduleExtension = "ydr",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadModels,
     .registrationOrder = 1,
     .knownRscVersion = 165},
    {.type = AssetType::DrawableDictionary,
     .extension = "ydd",
     .moduleExtension = "ydd",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadModels,
     .registrationOrder = 2,
     .knownRscVersion = 165},
    {.type = AssetType::Fragment,
     .extension = "yft",
     .moduleExtension = "yft",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadModels,
     .registrationOrder = 3,
     .knownRscVersion = 162},
    {.type = AssetType::MapTypes,
     .extension = "ytyp",
     .moduleExtension = "ytyp",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Late,
     .gate = ConfigGate::LoadMaps,
     .registrationOrder = 0,
     .knownRscVersion = 2},
    {.type = AssetType::StaticBounds,
     .extension = "ybn",
     .moduleExtension = "ybn",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Late,
     .gate = ConfigGate::LoadCollisions,
     .registrationOrder = 1,
     .knownRscVersion = 43},
    {.type = AssetType::MapData,
     .extension = "ymap",
     .moduleExtension = "ymap",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Late,
     .gate = ConfigGate::LoadMaps,
     .registrationOrder = 2,
     .knownRscVersion = 2},
    // Never a streaming object: a .ymf goes through the manifest chunk loader, so it
    // has no streaming module of its own. Most are PSO files ("PSIN"), some are compiled.
    {.type = AssetType::PackfileManifest,
     .extension = "ymf",
     .moduleExtension = "",
     .expectsRscHeader = true,
     .acceptsPsoMetadata = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Late,
     .gate = ConfigGate::LoadManifests,
     .registrationOrder = 3,
     .knownRscVersion = 0},
    {.type = AssetType::ClipDictionary,
     .extension = "ycd",
     .moduleExtension = "ycd",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadAnimations,
     .registrationOrder = 4,
     .knownRscVersion = 46},
    // Ped metadata, above all a clothing pack's drawable collection
    // (mp_f_freemode_01_<collection>.ymt), which the SHOP_PED_APPAREL_META_FILE names. It has
    // to be registered before that data file is mounted, which the early stage guarantees.
    {.type = AssetType::Metadata,
     .extension = "ymt",
     .moduleExtension = "ymt",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadModels,
     .registrationOrder = 5,
     .knownRscVersion = 2},
    {.type = AssetType::Navmesh,
     .extension = "ynv",
     .moduleExtension = "ynv",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadMaps,
     .registrationOrder = 4,
     .knownRscVersion = 0},
    {.type = AssetType::PathNodes,
     .extension = "ynd",
     .moduleExtension = "ynd",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::LoadMaps,
     .registrationOrder = 4,
     .knownRscVersion = 0},
    {.type = AssetType::ParticleFx,
     .extension = "ypt",
     .moduleExtension = "ypt",
     .expectsRscHeader = true,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::Always,
     .registrationOrder = 4,
     .knownRscVersion = 0},
    // Scaleform movies are plain files, not RAGE resources: no RSC header to validate.
    {.type = AssetType::Scaleform,
     .extension = "gfx",
     .moduleExtension = "gfx",
     .expectsRscHeader = false,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::Always,
     .registrationOrder = 4,
     .knownRscVersion = 0},
    // No store of its own in this table: RegisterAsset asks the game for one named after the
    // file's extension, and skips the file when there is none (FiveM LoadStreamingFile.cpp:2022).
    {.type = AssetType::OtherModule,
     .extension = "other",
     .moduleExtension = "",
     .expectsRscHeader = false,
     .tier = SupportTier::Supported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::Always,
     .registrationOrder = 6,
     .knownRscVersion = 0},
    {.type = AssetType::Unknown,
     .extension = "unknown",
     .moduleExtension = "",
     .expectsRscHeader = false,
     .tier = SupportTier::Unsupported,
     .stage = RegistrationStage::Early,
     .gate = ConfigGate::Always,
     .registrationOrder = 7,
     .knownRscVersion = 0},
}};

static_assert(kAssetTypes.back().type == AssetType::Unknown, "Unknown must stay the last row");
} // namespace

AssetType Classify(std::string_view extension)
{
    if (extension.starts_with('.'))
    {
        extension.remove_prefix(1);
    }
    if (extension.empty())
    {
        return AssetType::Unknown;
    }

    const std::string lowered = util::ToLower(extension);
    const auto match = std::ranges::find_if(kAssetTypes,
                                            [&lowered](const AssetTypeInfo& info)
                                            {
                                                return info.type != AssetType::Unknown &&
                                                       info.type != AssetType::OtherModule &&
                                                       info.extension == lowered;
                                            });
    return match != kAssetTypes.end() ? match->type : AssetType::Unknown;
}

const AssetTypeInfo& GetAssetTypeInfo(AssetType type)
{
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= kAssetTypes.size())
    {
        return kAssetTypes.back(); // Unknown
    }

    const AssetTypeInfo& info = kAssetTypes[index];
    // The table is indexed by the enumerator, so a row out of place would silently describe
    // the wrong type.
    return info.type == type ? info : kAssetTypes.back();
}

std::span<const AssetTypeInfo> GetAssetTypes()
{
    return kAssetTypes;
}

std::string_view GetModuleExtension(AssetType type, std::string_view fileExtension)
{
    return type == AssetType::OtherModule ? fileExtension : GetAssetTypeInfo(type).moduleExtension;
}

std::string_view ToString(AssetType type)
{
    return GetAssetTypeInfo(type).extension;
}

bool IsRegistrationImplemented(AssetType type)
{
    // Everything but a .ymf, which goes through the Manifests stage instead.
    constexpr std::array kImplemented = {
        AssetType::TextureDictionary, AssetType::Drawable,       AssetType::DrawableDictionary,
        AssetType::Fragment,          AssetType::ClipDictionary, AssetType::MapTypes,
        AssetType::StaticBounds,      AssetType::MapData,        AssetType::Metadata,
        AssetType::Navmesh,           AssetType::PathNodes,      AssetType::ParticleFx,
        AssetType::Scaleform,         AssetType::OtherModule};
    return std::ranges::find(kImplemented, type) != kImplemented.end();
}

bool IsGateOpen(ConfigGate gate, const config::StreamingSettings& settings)
{
    switch (gate)
    {
        using enum ConfigGate;
    case Always:
        return true;
    case LoadTextures:
        return settings.loadTextures;
    case LoadModels:
        return settings.loadModels;
    case LoadMaps:
        return settings.loadMaps;
    case LoadCollisions:
        return settings.loadCollisions;
    case LoadManifests:
        return settings.loadManifests;
    case LoadAnimations:
        return settings.loadAnimations;
    }
    return true;
}

std::string_view ToString(ConfigGate gate)
{
    switch (gate)
    {
        using enum ConfigGate;
    case Always:
        return "";
    case LoadTextures:
        return "load_textures";
    case LoadModels:
        return "load_models";
    case LoadMaps:
        return "load_maps";
    case LoadCollisions:
        return "load_collisions";
    case LoadManifests:
        return "load_manifests";
    case LoadAnimations:
        return "load_animations";
    }
    return "";
}
} // namespace spl::streaming
