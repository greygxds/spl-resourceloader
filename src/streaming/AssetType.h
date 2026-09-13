#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "config/LoaderConfig.h"

namespace spl::streaming
{
/// A file type the game can stream. One enumerator per extension we know something about;
/// OtherModule stands for every other extension, whose store is looked up at registration.
enum class AssetType : uint8_t
{
    TextureDictionary,  ///< ytd
    Drawable,           ///< ydr
    DrawableDictionary, ///< ydd
    Fragment,           ///< yft
    MapTypes,           ///< ytyp
    StaticBounds,       ///< ybn
    MapData,            ///< ymap
    PackfileManifest,   ///< ymf
    ClipDictionary,     ///< ycd
    Metadata,           ///< ymt
    Navmesh,            ///< ynv
    PathNodes,          ///< ynd
    ParticleFx,         ///< ypt
    Scaleform,          ///< gfx
    OtherModule, ///< any other extension: registered when the game has a store for it, as FiveM
                 ///< does
    Unknown
};

/// Unknown is the last enumerator, so this is also the size of a per-type counter array.
constexpr std::size_t kAssetTypeCount = static_cast<std::size_t>(AssetType::Unknown) + 1;

/// How far support for a type has come. A type that is not Supported is still discovered and
/// listed, so users can see why their file was not loaded.
enum class SupportTier : uint8_t
{
    Supported,  ///< registered by the loader
    Planned,    ///< recognized, registration comes later
    Unsupported ///< not a streaming extension we know
};

/// When an asset is registered. Mirrors FiveM's LoadType deferral: the types that map data
/// refers to have to be known before the maps that use them.
enum class RegistrationStage : uint8_t
{
    Early, ///< textures and models
    Late   ///< ytyp, ybn, ymap, and the ymf manifests
};

/// The [streaming] flag that switches a type off.
enum class ConfigGate : uint8_t
{
    Always, ///< no flag: on as soon as the type is supported
    LoadTextures,
    LoadModels,
    LoadMaps,
    LoadCollisions,
    LoadManifests,
    LoadAnimations
};

/// Everything the loader knows about one asset type, from one table (AssetType.cpp).
struct AssetTypeInfo
{
    AssetType type = AssetType::Unknown;
    std::string_view extension; ///< "ytd", without the dot
    std::string_view
        moduleExtension; ///< RAGE streaming module to look up; empty for ymf and OtherModule
    bool expectsRscHeader = false;   ///< every type except gfx
    bool acceptsPsoMetadata = false; ///< a "PSIN" file passes the header check too (ymf)
    SupportTier tier = SupportTier::Unsupported;
    RegistrationStage stage = RegistrationStage::Early;
    ConfigGate gate = ConfigGate::Always;

    /// Order inside the stage, lowest first: ytyp before ybn before ymap.
    uint8_t registrationOrder = 0;

    /// The resource version seen in practice for this type, or 0 when we have no reference
    /// value. Informational only: a mismatch is a warning, never a rejection.
    uint32_t knownRscVersion = 0;
};

/// The extension without its dot, in any case, mapped to a type: "YDR", ".ydr" and "ydr" all
/// give Drawable. Unknown for anything else.
[[nodiscard]] AssetType Classify(std::string_view extension);

/// The table row for a type. Always valid, Unknown included.
[[nodiscard]] const AssetTypeInfo& GetAssetTypeInfo(AssetType type);

/// Every row, in registration order within each stage. Unknown is the last entry.
[[nodiscard]] std::span<const AssetTypeInfo> GetAssetTypes();

/// The store an asset registers into: the table's, or for OtherModule the file's own extension
/// ("ysc" for a .ysc). Empty for a .ymf.
[[nodiscard]] std::string_view GetModuleExtension(AssetType type, std::string_view fileExtension);

/// The extension, for logs and counters: "ydr", or "unknown".
[[nodiscard]] std::string_view ToString(AssetType type);

/// True when the loader can actually register this type today. Support is built one type at
/// a time, so a type that is Supported in the table above may still be waiting for the code
/// that registers it: textures first, models and maps after. Not configurable — a user who
/// switches this off has nothing to gain and a broken world to lose.
[[nodiscard]] bool IsRegistrationImplemented(AssetType type);

/// True when the [streaming] settings enable this type's gate.
[[nodiscard]] bool IsGateOpen(ConfigGate gate, const config::StreamingSettings& settings);

/// The config key behind a gate, for the message that explains a skip: "load_maps".
[[nodiscard]] std::string_view ToString(ConfigGate gate);
} // namespace spl::streaming
