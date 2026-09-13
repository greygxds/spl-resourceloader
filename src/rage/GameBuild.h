#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace spl::rage
{
/// Which GTA V the loader is running in. Only Legacy is supported: Enhanced is a different
/// executable with different layouts, and every signature we have would resolve to nonsense.
enum class Edition
{
    Legacy,
    Enhanced,
    Unknown
};

[[nodiscard]] std::string_view ToString(Edition edition);

/// The game we are inside of, as read from the executable itself. The exe version is
/// authoritative; ScriptHookV's own opinion is logged next to it as a cross-check.
struct GameBuild
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0; ///< the number builds are known by: 3258 for 1.0.3258.0
    uint32_t revision = 0;
    Edition edition = Edition::Unknown;
    std::string distribution;   ///< "Steam", "Epic", "Rockstar" or "unknown"; informational only
    std::string executableName; ///< "GTA5.exe"

    /// "1.0.3258.0 (Legacy, Steam, GTA5.exe)"
    [[nodiscard]] std::string ToString() const;

    /// False for anything but Legacy, which keeps the RAGE bridge disabled with a clear
    /// message instead of guessing at addresses.
    [[nodiscard]] bool IsSupported() const
    {
        return edition == Edition::Legacy;
    }
};

/// Reads the version resource of the running executable. std::nullopt when it carries no
/// version information at all, which means we cannot tell builds apart and must not hook.
[[nodiscard]] std::optional<GameBuild> DetectGameBuild();

/// The same for an executable on disk, which is what spl_sigcheck inspects. Free of
/// ScriptHookV, like everything spl_sigcheck compiles.
[[nodiscard]] std::optional<GameBuild> ReadGameBuild(const std::filesystem::path& executable);

[[nodiscard]] bool IsBuildAtLeast(const GameBuild& gameBuild, uint32_t buildNumber);

/// How a build relates to the builds somebody ran the loader on.
enum class BuildVerification
{
    Verified,
    OlderUnverified, ///< not on record, but no newer than the newest verified build
    NewerUnverified  ///< newer than every verified build: a game update nobody has checked yet
};

[[nodiscard]] std::string_view ToString(BuildVerification verification);

[[nodiscard]] BuildVerification ClassifyBuild(uint32_t buildNumber);

/// The newest build on record.
[[nodiscard]] uint32_t GetNewestVerifiedBuild();
} // namespace spl::rage
