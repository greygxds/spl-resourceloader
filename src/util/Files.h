#pragma once

#include <filesystem>
#include <string_view>

namespace spl::util
{
/// Writes contents next to file and renames it over file, so a crash mid-write leaves the old
/// contents rather than a truncated file. Returns false, and leaves file untouched, on failure.
[[nodiscard]] bool WriteFileAtomically(const std::filesystem::path& file,
                                       std::string_view contents);
} // namespace spl::util
