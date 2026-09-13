#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config/LoaderConfig.h"

namespace spl::config
{
/// Problems found while reading the configuration. They are collected rather than logged,
/// because the logger is not configured yet at that point; Application replays them once it is.
struct ConfigDiagnostics
{
    std::vector<std::string> warnings; ///< recoverable: the default is kept for that key
    std::vector<std::string> errors;   ///< the file could not be used at all

    [[nodiscard]] bool IsEmpty() const
    {
        return warnings.empty() && errors.empty();
    }
};

struct ConfigLoadResult
{
    LoaderConfig config;
    ConfigDiagnostics diagnostics;
    bool wroteDefault = false; ///< the file was missing and the default was written
};

/// Reads config.toml into a LoaderConfig. Nothing here throws: toml++ exceptions are caught
/// and turned into diagnostics, and a bad file always degrades to the built-in defaults.
class ConfigLoader
{
public:
    /// Parses file, writing the commented default first when it does not exist.
    /// A file that fails to parse is left untouched and the defaults are used.
    [[nodiscard]] static ConfigLoadResult LoadOrCreate(const std::filesystem::path& file);

    /// Parses TOML text. Pure, so it carries the unit tests.
    [[nodiscard]] static ConfigLoadResult Parse(std::string_view tomlText);
};

/// Canonical lower-case name of a level, as written in config.toml.
[[nodiscard]] std::string_view ToString(LogLevel level);
} // namespace spl::config
