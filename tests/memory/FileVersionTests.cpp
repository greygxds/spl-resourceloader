#include <filesystem>
#include <optional>
#include <string>

#include <catch_amalgamated.hpp>

#include "memory/FileVersion.h"
#include "memory/Module.h"

using namespace spl::memory;

namespace
{
/// A Windows system binary is the one file we can rely on having a version resource, which
/// is the same mechanism GTA5.exe's build number is read through.
[[nodiscard]] std::filesystem::path SystemBinary(const std::string& fileName)
{
    const std::optional<Module> kernel = Module::Find(L"kernel32.dll");
    REQUIRE(kernel.has_value());
    return kernel->GetPath().parent_path() / fileName;
}
} // namespace

TEST_CASE("FileVersion: reads the version resource of a system binary", "[memory]")
{
    const std::optional<FileVersion> version = ReadFileVersion(SystemBinary("notepad.exe"));
    REQUIRE(version.has_value());

    // Windows 10 and 11 both report 10.x here; the point is that all four parts were read.
    CHECK(version->major >= 6);
    CHECK(version->build > 0);
    CHECK(version->ToString() ==
          std::to_string(version->major) + "." + std::to_string(version->minor) + "." +
              std::to_string(version->build) + "." + std::to_string(version->revision));
}

TEST_CASE("FileVersion: a file without a version resource is nullopt", "[memory]")
{
    CHECK_FALSE(ReadFileVersion("C:/spl-no-such-file.exe").has_value());
}

TEST_CASE("FileVersion: two reads of the same file agree", "[memory]")
{
    const std::filesystem::path binary = SystemBinary("kernel32.dll");

    CHECK(ReadFileVersion(binary) == ReadFileVersion(binary));
}
