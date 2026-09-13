#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "platform/Win32.h"

namespace spl::tests
{
/// A unique directory under the system temp folder, removed when the test ends.
/// Tests never touch the real game directory (conventions section 9).
class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("spl_tests_" + std::to_string(::GetCurrentProcessId()) + "_" +
                  std::to_string(counter++));

        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::create_directories(m_path, error);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const
    {
        return m_path;
    }

    /// Creates a file, and any parent directories it needs. Paths are relative to Path().
    void WriteFile(std::string_view relativePath, std::string_view contents) const
    {
        const std::filesystem::path file = m_path / relativePath;
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);

        std::ofstream stream{file, std::ios::binary | std::ios::trunc};
        stream << contents;
    }

    [[nodiscard]] std::string ReadFile(std::string_view relativePath) const
    {
        std::ifstream stream{m_path / relativePath, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    }

    void AddDirectory(std::string_view relativePath) const
    {
        std::error_code error;
        std::filesystem::create_directories(m_path / relativePath, error);
    }

private:
    std::filesystem::path m_path;
};

/// A temp directory shaped like a resources root, for the discovery tests.
class TempTree : public TempDir
{
public:
    [[nodiscard]] const std::filesystem::path& Root() const
    {
        return Path();
    }

    /// A resource folder with an fxmanifest.lua. relativePath may include categories,
    /// e.g. "[maps]/city".
    void AddResource(std::string_view relativePath) const
    {
        WriteFile(std::string{relativePath} + "/fxmanifest.lua",
                  "fx_version 'cerulean'\ngame 'gta5'\n");
    }

    /// A resource folder with only the legacy __resource.lua.
    void AddLegacyResource(std::string_view relativePath) const
    {
        WriteFile(std::string{relativePath} + "/__resource.lua",
                  "resource_manifest_version '44'\n");
    }
};
} // namespace spl::tests
