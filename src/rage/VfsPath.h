#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "rage/types/DataFileTypes.h"
#include "util/Strings.h"

namespace spl::rage
{
/// The mount point every resource file is reached through. Lowercase, ASCII and unique, so it
/// cannot collide with a game or DLC mount.
constexpr std::string_view kResourcesMountPoint = "splres:/";

/// The mount point user mods are served at, straight from their archives. A separate mount,
/// so resource VFS paths never shift when mods come and go.
constexpr std::string_view kModsMountPoint = "splmods:/";

/// The root string fiDeviceRelative::SetPath wants: UTF-8, forward slashes, trailing slash.
/// Pure: it never touches the filesystem, so the caller decides what "absolute" means.
[[nodiscard]] inline std::string MakeDeviceRoot(const std::filesystem::path& root)
{
    std::string text = util::ToUtf8Generic(root);
    while (!text.empty() && text.back() == '/')
    {
        text.pop_back();
    }
    text += '/';
    return text;
}

/// "splres:/[maps]/example/stream/spl_test.ytd" for a file under root, or std::nullopt when
/// the file is not under it. The case on disk is kept: RAGE may lower-case during path
/// normalization, and a mismatch would then be invisible rather than diagnosable.
[[nodiscard]] inline std::optional<std::string>
MakeVfsPath(const std::filesystem::path& root, std::string_view mountPoint,
            const std::filesystem::path& absoluteFile)
{
    const std::filesystem::path relative = absoluteFile.lexically_relative(root);
    if (relative.empty() || relative == std::filesystem::path{"."})
    {
        return std::nullopt;
    }
    if (*relative.begin() == std::filesystem::path{".."})
    {
        return std::nullopt; // outside the mounted root, so no VFS path can name it
    }

    std::string text{mountPoint};
    text += util::ToUtf8Generic(relative);
    return text;
}

/// The resources-root form, for the callers that only ever mount that one.
[[nodiscard]] inline std::optional<std::string>
MakeVfsPath(const std::filesystem::path& root, const std::filesystem::path& absoluteFile)
{
    return MakeVfsPath(root, kResourcesMountPoint, absoluteFile);
}

/// Prefix for a data-file name whose VFS path does not fit the game's entry.
constexpr std::string_view kShortDataFilePrefix = "spl/";

/// Whether a data-file entry may carry a shortened name instead of the path.
enum class DataFileNaming : uint8_t
{
    PathOnly,        ///< the mounter opens the file: every type but DLC_ITYP_REQUEST
    BaseNameIsEnough ///< the ytyp mounter only reads the base name
};

/// The name a data-file entry carries: the VFS path when it fits. Otherwise, only when the
/// mounter reads nothing but the base name, "spl/<file name>" (FiveM unloads a ytyp with
/// "dummy/<name>.ityp"). std::nullopt when neither fits.
[[nodiscard]] inline std::optional<std::string>
MakeDataFileEntryName(std::string_view vfsPath, DataFileNaming naming,
                      std::size_t maxLength = DataFileLayout::kMaxNameLength)
{
    if (vfsPath.size() <= maxLength)
    {
        return std::string{vfsPath};
    }
    if (naming == DataFileNaming::PathOnly)
    {
        return std::nullopt; // a shortened path names a file that does not exist
    }

    const std::size_t slash = vfsPath.find_last_of('/');
    const std::string_view fileName =
        slash == std::string_view::npos ? vfsPath : vfsPath.substr(slash + 1);
    if (fileName.empty() || kShortDataFilePrefix.size() + fileName.size() > maxLength)
    {
        return std::nullopt;
    }

    std::string name{kShortDataFilePrefix};
    name += fileName;
    return name;
}

/// True when every byte is printable ASCII. RAGE devices in V take UTF-8, but nothing has
/// proven that its path normalization handles non-ASCII, so callers warn instead of failing.
[[nodiscard]] inline bool IsPrintableAscii(std::string_view text)
{
    return std::ranges::all_of(text, [](char character)
                               { return character >= 0x20 && character < 0x7F; });
}
} // namespace spl::rage
