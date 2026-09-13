#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace spl::util
{
/// Matches a path against a glob pattern, case-insensitively, on '/'-separated text.
///
/// `*` matches anything inside one segment, `?` matches one character inside one segment, and
/// a `**` segment matches any number of segments, none included. Pure, so it carries the tests.
[[nodiscard]] bool MatchesGlob(std::string_view pattern, std::string_view path);

/// Every file under root matching pattern, as relative paths with forward slashes, sorted and
/// deduplicated (FiveM collects glob results in a std::set, so ordering is stable there too).
/// A pattern that escapes the root, or a root that cannot be read, yields nothing.
[[nodiscard]] std::vector<std::string> GlobFiles(const std::filesystem::path& root,
                                                 std::string_view pattern);

/// True when the pattern has no wildcard and therefore names a single file.
[[nodiscard]] bool IsLiteralPattern(std::string_view pattern);
} // namespace spl::util
