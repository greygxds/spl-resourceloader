#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace spl::util
{
/// A path as UTF-8. Filesystem paths are UTF-16 on Windows, and path::string() would try the
/// active code page and throw on anything it cannot represent; u8string() always succeeds.
[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path);

/// A path as UTF-8 with forward slashes, which is the spelling used for relative paths in
/// manifests, VFS paths and logs.
[[nodiscard]] std::string ToUtf8Generic(const std::filesystem::path& path);

/// ASCII lower-case. Resource names, asset names and config keywords are ASCII by
/// construction, so no locale is involved and the result is stable across machines.
[[nodiscard]] std::string ToLower(std::string_view text);

/// ASCII upper-case, for values the game compares in upper case (data-file type names).
[[nodiscard]] std::string ToUpper(std::string_view text);

/// Removes leading and trailing ASCII whitespace.
[[nodiscard]] std::string Trim(std::string_view text);

/// ASCII case-insensitive comparison, the way resource and asset names are matched.
[[nodiscard]] bool EqualsIgnoreCase(std::string_view left, std::string_view right);
} // namespace spl::util
