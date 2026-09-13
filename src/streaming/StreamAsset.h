#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "resource/Resource.h"
#include "streaming/AssetType.h"
#include "streaming/RscHeader.h"

namespace spl::streaming
{
/// What the loader decided to do with one discovered file. Everything that is not Planned
/// carries a reason, so a user can see why their file did not load.
enum class AssetDisposition : uint8_t
{
    Planned,            ///< it goes into the streaming plan
    SkippedByConfig,    ///< a [streaming] flag switches its type off
    SkippedUnsupported, ///< the type is unknown, or support comes in a later version
    SkippedInvalid,     ///< the file is not a usable resource (no RSC header, unreadable)
    ShadowedByDuplicate ///< another asset of the same file name won
};

[[nodiscard]] constexpr std::string_view ToString(AssetDisposition disposition)
{
    switch (disposition)
    {
        using enum AssetDisposition;
    case Planned:
        return "planned";
    case SkippedByConfig:
        return "skipped by configuration";
    case SkippedUnsupported:
        return "unsupported";
    case SkippedInvalid:
        return "invalid";
    case ShadowedByDuplicate:
        return "shadowed by a duplicate";
    }
    return "unsupported";
}

/// One file found under a resource's stream/ folder.
struct StreamAsset
{
    resource::ResourceId owner;
    std::string resourceName; ///< so logs need no lookup

    std::filesystem::path absolutePath;
    std::string relativePath; ///< "stream/props/prop_a.ydr", forward slashes, case as on disk
    /// "prop_a.ydr", lower-case: the name the game knows the file by, which for a ped
    /// component ("collection^accs_000_u.ydd") is "collection/accs_000_u.ydd".
    std::string fileName;
    std::string streamingName; ///< fileName without its extension
    std::string extension;     ///< "ydr", lower-case, without the dot

    AssetType type = AssetType::Unknown;
    uint64_t fileSizeBytes = 0;
    std::filesystem::file_time_type lastWriteTime{}; ///< lets a changed file be noticed

    /// The file's RSC header, or std::nullopt when it was not read: an unreadable file, or a
    /// type that carries no header (.gfx).
    std::optional<RscHeader> rsc;

    AssetDisposition disposition = AssetDisposition::Planned;
    std::string dispositionReason; ///< empty while the disposition is Planned
};
} // namespace spl::streaming
