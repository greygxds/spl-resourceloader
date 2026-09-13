#include "rage/GameBuild.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "memory/FileVersion.h"
#include "memory/Module.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
/// Builds somebody ran the loader on. A build goes here once every signature resolved on it
/// and registered textures and props loaded in game.
constexpr std::array<uint32_t, 2> kVerifiedBuilds{3411, 3889};

/// The Enhanced executable is a different game; anything else is a repack or a launcher we
/// know nothing about, and is treated as unsupported for the same reason.
[[nodiscard]] Edition DetectEdition(std::string_view executableName)
{
    if (util::EqualsIgnoreCase(executableName, "GTA5.exe"))
    {
        return Edition::Legacy;
    }
    if (util::EqualsIgnoreCase(executableName, "GTA5_Enhanced.exe"))
    {
        return Edition::Enhanced;
    }
    return Edition::Unknown;
}

/// Which store the copy came from, guessed from the SDK each one ships next to the exe.
/// Purely informational: it appears in the log so a bug report says where the copy is from.
[[nodiscard]] std::string DetectDistribution(const std::filesystem::path& gameDir)
{
    struct Marker
    {
        std::string_view fileName;
        std::string_view distribution;
    };
    static constexpr std::array kMarkers = {
        Marker{"steam_api64.dll", "Steam"},
        Marker{"EOSSDK-Win64-Shipping.dll", "Epic"},
        Marker{"Launcher.exe", "Rockstar"},
    };

    std::error_code error;
    for (const Marker& marker : kMarkers)
    {
        if (std::filesystem::exists(gameDir / marker.fileName, error))
        {
            return std::string(marker.distribution);
        }
    }
    return "unknown";
}
} // namespace

std::string_view ToString(Edition edition)
{
    using enum Edition;
    switch (edition)
    {
    case Legacy:
        return "Legacy";
    case Enhanced:
        return "Enhanced";
    case Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string GameBuild::ToString() const
{
    return fmt::format("{}.{}.{}.{} ({}, {}, {})", major, minor, build, revision,
                       rage::ToString(edition), distribution, executableName);
}

std::optional<GameBuild> DetectGameBuild()
{
    const memory::Module main = memory::Module::Main();
    const std::filesystem::path& executable = main.GetPath();
    if (executable.empty())
    {
        SPL_LOG_ERROR(Rage, "Could not determine the path of the running executable");
        return std::nullopt;
    }
    return ReadGameBuild(executable);
}

std::optional<GameBuild> ReadGameBuild(const std::filesystem::path& executable)
{
    const std::string fileName = util::ToUtf8(executable.filename());
    const std::optional<memory::FileVersion> version = memory::ReadFileVersion(executable);
    if (!version)
    {
        SPL_LOG_ERROR(Rage, "'{}' carries no version resource, so the build is unknown", fileName);
        return std::nullopt;
    }

    return GameBuild{
        .major = version->major,
        .minor = version->minor,
        .build = version->build,
        .revision = version->revision,
        .edition = DetectEdition(fileName),
        .distribution = DetectDistribution(executable.parent_path()),
        .executableName = fileName,
    };
}

bool IsBuildAtLeast(const GameBuild& gameBuild, uint32_t buildNumber)
{
    return gameBuild.build >= buildNumber;
}

std::string_view ToString(BuildVerification verification)
{
    using enum BuildVerification;
    switch (verification)
    {
    case Verified:
        return "verified";
    case OlderUnverified:
        return "not verified";
    case NewerUnverified:
        return "newer than every verified build";
    }
    return "not verified";
}

BuildVerification ClassifyBuild(uint32_t buildNumber)
{
    if (std::ranges::find(kVerifiedBuilds, buildNumber) != kVerifiedBuilds.end())
    {
        return BuildVerification::Verified;
    }
    return buildNumber > GetNewestVerifiedBuild() ? BuildVerification::NewerUnverified
                                                  : BuildVerification::OlderUnverified;
}

uint32_t GetNewestVerifiedBuild()
{
    return *std::ranges::max_element(kVerifiedBuilds);
}
} // namespace spl::rage
