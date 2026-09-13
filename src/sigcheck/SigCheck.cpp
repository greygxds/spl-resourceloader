// spl_sigcheck: resolves the signature table against a GTA5.exe, so a new game build can be
// checked without the loader.
//
//   spl_sigcheck <path to GTA5.exe> [--build <number>] [--bytes <count>]
//   spl_sigcheck --process [<process name>] [--build <number>] [--bytes <count>]
//
// A retail GTA5.exe is packed on disk, so --process, which copies the image out of a running
// game, is the mode that finds code signatures on a real install.
//
// Exit code 0 when every required signature resolved, 1 when one did not, 2 on bad input.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "core/Result.h"
#include "memory/ImageFile.h"
#include "memory/Module.h"
#include "rage/AddressResolver.h"
#include "rage/GameBuild.h"
#include "rage/signatures/SignatureSpec.h"
#include "rage/types/MapStoreTypes.h"
#include "util/Hash.h"
#include "util/Strings.h"

namespace
{
using namespace spl;

constexpr std::size_t kDefaultByteCount = 16;

struct Arguments
{
    std::filesystem::path executable;
    std::wstring processName; ///< non-empty for --process
    std::optional<uint32_t> build;
    std::size_t byteCount = kDefaultByteCount;
};

void PrintUsage()
{
    std::fputs(
        "usage: spl_sigcheck <path to GTA5.exe> [--build <number>] [--bytes <count>]\n"
        "       spl_sigcheck --process [<name, default GTA5.exe>] [--build ...] [--bytes ...]\n"
        "\n"
        "Resolves every signature the loader uses against the executable, or against the\n"
        "image of a running game with --process (a retail GTA5.exe is packed on disk), and\n"
        "prints match counts, offsets and the bytes at each address, to record a build or diff\n"
        "it against one recorded earlier.\n",
        stderr);
}

std::optional<uint32_t> ParseNumber(std::wstring_view text)
{
    uint32_t value = 0;
    if (text.empty())
    {
        return std::nullopt;
    }
    for (const wchar_t digit : text)
    {
        if (digit < L'0' || digit > L'9' || value > (UINT32_MAX - 9) / 10)
        {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint32_t>(digit - L'0');
    }
    return value;
}

std::optional<Arguments> ParseArguments(std::span<wchar_t*> arguments)
{
    Arguments parsed;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::wstring_view argument = arguments[index];
        const bool hasValue = index + 1 < arguments.size();
        if (argument == L"--build" && hasValue)
        {
            parsed.build = ParseNumber(arguments[++index]);
            if (!parsed.build)
            {
                return std::nullopt;
            }
        }
        else if (argument == L"--bytes" && hasValue)
        {
            const std::optional<uint32_t> count = ParseNumber(arguments[++index]);
            if (!count || *count == 0 || *count > 256)
            {
                return std::nullopt;
            }
            parsed.byteCount = *count;
        }
        else if (argument == L"--process")
        {
            parsed.processName = L"GTA5.exe";
            if (hasValue && !std::wstring_view{arguments[index + 1]}.starts_with(L"--"))
            {
                parsed.processName = arguments[++index];
            }
        }
        else if (argument.starts_with(L"--") || !parsed.executable.empty())
        {
            return std::nullopt;
        }
        else
        {
            parsed.executable = std::filesystem::path{argument};
        }
    }
    if (parsed.executable.empty() == parsed.processName.empty())
    {
        return std::nullopt; // exactly one of a path and --process
    }
    return parsed;
}

/// "48 8B 05 ..", or "outside the image" for an address a Direct row resolved outside it.
std::string HexAt(const memory::Module& module, uintptr_t address, std::size_t count)
{
    if (!module.Contains(address))
    {
        return "outside the image";
    }
    const std::size_t available = module.GetBase() + module.GetSizeBytes() - address;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    std::string text;
    for (std::size_t index = 0; index < std::min(count, available); ++index)
    {
        text += fmt::format("{}{:02X}", index == 0 ? "" : " ", bytes[index]);
    }
    return text;
}

void PrintLine(const std::string& line)
{
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
}

/// The change-set replay's patch sites, as they are recorded for a verified build.
void PrintChangeSetSites(const memory::Module& module, uintptr_t loadChangeSet, uint32_t build)
{
    using namespace rage::ChangeSetReplayLayout;
    if (!module.Contains(loadChangeSet) ||
        module.GetBase() + module.GetSizeBytes() - loadChangeSet < kFunctionBytes)
    {
        return;
    }
    const std::string_view function{reinterpret_cast<const char*>(loadChangeSet), kFunctionBytes};
    PrintLine("");
    PrintLine(fmt::format("build 1.0.{}.0, LoadChangeSet digest {:#010x}", build,
                          util::JoaatExact(function)));
    for (const Site& site : kSites)
    {
        PrintLine(fmt::format("LoadChangeSet+{:#x} ({}): {}", site.offset, site.purpose,
                              HexAt(module, loadChangeSet + site.offset, site.sizeBytes)));
    }
}

int Run(const Arguments& arguments)
{
    Result<memory::ImageFile> image = arguments.processName.empty()
                                          ? memory::ImageFile::Load(arguments.executable)
                                          : memory::ImageFile::Capture(arguments.processName);
    if (!image)
    {
        std::fprintf(stderr, "%s\n", image.GetError().message.c_str());
        return 2;
    }
    const memory::Module& module = image.GetValue().GetModule();
    const std::filesystem::path& executable = module.GetPath();

    const std::optional<rage::GameBuild> detected = rage::ReadGameBuild(executable);
    if (!detected && !arguments.build)
    {
        std::fprintf(stderr, "'%s' has no version resource; pass --build <number>\n",
                     util::ToUtf8(executable).c_str());
        return 2;
    }
    rage::GameBuild build = detected.value_or(rage::GameBuild{});
    if (arguments.build)
    {
        build.build = *arguments.build;
    }

    const rage::ResolvedSignatures resolved = rage::AddressResolver::ResolveAll(module, build);

    PrintLine(fmt::format("Executable: {}{}", util::ToUtf8(executable),
                          arguments.processName.empty() ? "" : " (running process)"));
    PrintLine(fmt::format("Build:      {} ({})", build.build,
                          rage::ToString(rage::ClassifyBuild(build.build))));
    PrintLine("");

    std::optional<uintptr_t> loadChangeSet;
    bool anyCodeResolved = false;
    for (const rage::SignatureSpec& spec : rage::AllSignatures())
    {
        const auto count = resolved.matchCounts.find(std::string{spec.name});
        if (count == resolved.matchCounts.end())
        {
            continue; // not applicable to this build, or a malformed pattern (listed below)
        }
        const std::optional<uintptr_t> address = resolved.Find(spec.name);
        const std::string where =
            address ? fmt::format("{}+{:#x}", module.GetFileName(), *address - module.GetBase())
                    : std::string{spec.required ? "MISSING" : "missing (optional)"};
        PrintLine(
            fmt::format("{:<55} {:>3} match(es)  {:<24} {}", spec.name, count->second, where,
                        address ? HexAt(module, *address, arguments.byteCount) : std::string{}));
        anyCodeResolved = anyCodeResolved || (address && spec.section == memory::SectionKind::Code);
        if (address && spec.name == "CFileLoader::LoadChangeSet")
        {
            loadChangeSet = address;
        }
    }

    if (loadChangeSet)
    {
        PrintChangeSetSites(module, *loadChangeSet, build.build);
    }

    PrintLine("");
    PrintLine(fmt::format("Resolved {}/{} signatures ({} optional missing) in {} ms",
                          resolved.addresses.size(), resolved.applicable, resolved.optionalMissing,
                          resolved.durationMs));
    for (const std::string& failure : resolved.failures)
    {
        PrintLine("FAILED: " + failure);
    }
    if (!anyCodeResolved && arguments.processName.empty())
    {
        PrintLine("");
        PrintLine("No code signature resolved. A retail GTA5.exe is packed on disk, so its code");
        PrintLine("only exists once the game runs: start the game and use --process instead.");
    }
    return resolved.IsComplete() ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::optional<Arguments> arguments =
        ParseArguments(std::span<wchar_t*>{argv + 1, static_cast<std::size_t>(argc - 1)});
    if (!arguments)
    {
        PrintUsage();
        return 2;
    }
    return Run(*arguments);
}
