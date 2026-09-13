#pragma once

#include <filesystem>
#include <optional>

namespace spl
{
/// Absolute locations the loader works with. Everything the loader writes stays inside
/// dataDir; the only file in gameDir is resourceLoader.asi.
struct Paths
{
    std::filesystem::path gameDir;    ///< folder holding GTA5.exe
    std::filesystem::path dataDir;    ///< <gameDir>/resourceLoader
    std::filesystem::path configFile; ///< <dataDir>/config.toml

    /// Loader-owned state, never edited by the loader in config.toml.
    std::filesystem::path stateFile;       ///< <dataDir>/state.toml: quarantined resources
    std::filesystem::path markerFile;      ///< <dataDir>/running.marker: registration under way
    std::filesystem::path crashReportFile; ///< <dataDir>/crash.txt
    std::filesystem::path crashDumpFile;   ///< <dataDir>/crash.dmp

    /// Derives every path from the directory of the running executable.
    /// std::nullopt when the executable path cannot be determined.
    [[nodiscard]] static std::optional<Paths> Resolve();

    /// Derives every path from an explicit game directory. Used by tests.
    [[nodiscard]] static Paths FromGameDir(const std::filesystem::path& gameDir);

    /// Resolves a path taken from config.toml: relative paths are relative to the data folder,
    /// absolute paths are used as they are. Both are normalized, so the log shows one spelling.
    [[nodiscard]] std::filesystem::path ResolveUserPath(const std::filesystem::path& path) const;

    /// Creates the data folder if it does not exist. False when it cannot be created, which
    /// leaves the loader with nowhere to write and is treated as fatal.
    [[nodiscard]] bool EnsureDataDir() const;
};
} // namespace spl
