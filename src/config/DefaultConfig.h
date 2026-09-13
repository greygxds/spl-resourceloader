#pragma once

#include <filesystem>
#include <string_view>

namespace spl::config
{
/// The commented default config.toml, written verbatim when the file is missing.
///
/// Comment style: comment only what is not obvious from the key name, at most one line,
/// preferably inline after the value. The full reference for every key lives in the user
/// guide, not in this file.
[[nodiscard]] std::string_view DefaultConfigText();

/// Writes DefaultConfigText() to file, creating parent directories as needed.
/// Returns false when the file cannot be written; an existing file is never overwritten.
[[nodiscard]] bool WriteDefaultConfig(const std::filesystem::path& file);
} // namespace spl::config
