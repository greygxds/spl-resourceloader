#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spl::config
{
/// A user's config.toml with the options of a newer default added.
struct ConfigUpgrade
{
    std::string text;
    std::vector<std::string> addedOptions; ///< "table.key", in default order; empty: text unchanged
};
/// The commented default config.toml, written verbatim when the file is missing.
///
/// Comment style: comment only what is not obvious from the key name, at most one line,
/// preferably inline after the value. The full reference for every key lives in the user
/// guide, not in this file.
[[nodiscard]] std::string_view DefaultConfigText();

/// Writes DefaultConfigText() to file, creating parent directories as needed.
/// Returns false when the file cannot be written; an existing file is never overwritten.
[[nodiscard]] bool WriteDefaultConfig(const std::filesystem::path& file);

/// Adds every table and key of defaultText that userText lacks, as their lines from defaultText
/// with their comments: a key goes after the last value of its table, a table goes at the end.
/// Nothing the user wrote is changed, moved or removed. Pure, so it carries the tests.
///
/// std::nullopt when either text does not parse; a table the user wrote inline, or as something
/// other than a table, is left alone.
[[nodiscard]] std::optional<ConfigUpgrade> AddMissingOptions(std::string_view userText,
                                                             std::string_view defaultText);
} // namespace spl::config
