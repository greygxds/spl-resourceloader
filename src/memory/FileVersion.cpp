#include "memory/FileVersion.h"

#include <vector>

#include <spdlog/fmt/fmt.h>

#include "platform/Win32.h"

// The version API lives in its own import library, and nothing else in the solution needs it.
#pragma comment(lib, "version.lib")

namespace spl::memory
{
std::string FileVersion::ToString() const
{
    return fmt::format("{}.{}.{}.{}", major, minor, build, revision);
}

std::optional<FileVersion> ReadFileVersion(const std::filesystem::path& file)
{
    const std::wstring path = file.wstring();
    DWORD handle = 0;
    const DWORD sizeBytes = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (sizeBytes == 0)
    {
        return std::nullopt;
    }

    std::vector<std::byte> buffer(sizeBytes);
    if (::GetFileVersionInfoW(path.c_str(), handle, sizeBytes, buffer.data()) == 0)
    {
        return std::nullopt;
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSizeBytes = 0;
    if (::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoSizeBytes) ==
            0 ||
        info == nullptr || infoSizeBytes < sizeof(VS_FIXEDFILEINFO))
    {
        return std::nullopt;
    }

    return FileVersion{
        .major = HIWORD(info->dwFileVersionMS),
        .minor = LOWORD(info->dwFileVersionMS),
        .build = HIWORD(info->dwFileVersionLS),
        .revision = LOWORD(info->dwFileVersionLS),
    };
}
} // namespace spl::memory
