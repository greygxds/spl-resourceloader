#include "core/Paths.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "platform/Win32.h"

namespace spl
{
namespace
{
constexpr std::wstring_view kDataFolderName = L"resourceLoader";
constexpr std::wstring_view kConfigFileName = L"config.toml";

/// Full path of the running executable. std::nullopt when Win32 refuses to tell us.
std::optional<std::filesystem::path> FindExecutablePath()
{
    // GetModuleFileNameW truncates instead of failing on a short buffer, so grow until it fits.
    std::wstring buffer(MAX_PATH, L'\0');
    while (true)
    {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
        {
            return std::nullopt;
        }
        if (written < buffer.size())
        {
            buffer.resize(written);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2);
    }
}
} // namespace

Paths Paths::FromGameDir(const std::filesystem::path& gameDir)
{
    const std::filesystem::path dataDir = gameDir / kDataFolderName;
    return Paths{
        .gameDir = gameDir,
        .dataDir = dataDir,
        .configFile = dataDir / kConfigFileName,
        .stateFile = dataDir / L"state.toml",
        .markerFile = dataDir / L"running.marker",
        .crashReportFile = dataDir / L"crash.txt",
        .crashDumpFile = dataDir / L"crash.dmp",
    };
}

std::optional<Paths> Paths::Resolve()
{
    const std::optional<std::filesystem::path> executable = FindExecutablePath();
    if (!executable)
    {
        return std::nullopt;
    }
    return FromGameDir(executable->parent_path());
}

std::filesystem::path Paths::ResolveUserPath(const std::filesystem::path& path) const
{
    const std::filesystem::path combined = path.is_absolute() ? path : dataDir / path;

    // weakly_canonical works on paths that do not exist yet, which is the normal case for the
    // log file and for a resources folder the user has not created.
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(combined, error);
    return error ? combined.lexically_normal() : canonical;
}

bool Paths::EnsureDataDir() const
{
    std::error_code error;
    std::filesystem::create_directories(dataDir, error);
    if (error)
    {
        return false;
    }
    return std::filesystem::is_directory(dataDir, error);
}
} // namespace spl
