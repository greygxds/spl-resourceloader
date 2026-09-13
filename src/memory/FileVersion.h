#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace spl::memory
{
/// The four-part version from a PE file's VS_FIXEDFILEINFO resource. GTA5.exe reports its
/// build here, which is the authoritative way to tell builds apart.
struct FileVersion
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    uint32_t revision = 0;

    /// "1.0.3258.0"
    [[nodiscard]] std::string ToString() const;

    [[nodiscard]] friend bool operator==(const FileVersion&, const FileVersion&) = default;
};

/// Reads the version resource of an executable or DLL on disk. std::nullopt when the file
/// has no version resource, which is normal and not an error.
[[nodiscard]] std::optional<FileVersion> ReadFileVersion(const std::filesystem::path& file);
} // namespace spl::memory
