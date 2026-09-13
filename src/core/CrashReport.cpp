#include "core/CrashReport.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

namespace spl
{
namespace
{
struct ExceptionName
{
    uint32_t code;
    std::string_view name;
};

// The values are the NTSTATUS codes from winnt.h, spelled out so this file needs no Windows
// header (conventions section 7).
constexpr std::array kExceptionNames = {
    ExceptionName{0xC0000005, "EXCEPTION_ACCESS_VIOLATION"},
    ExceptionName{0xC000001D, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    ExceptionName{0xC0000094, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    ExceptionName{0xC00000FD, "EXCEPTION_STACK_OVERFLOW"},
    ExceptionName{0xC0000374, "STATUS_HEAP_CORRUPTION"},
    ExceptionName{0xC0000409, "STATUS_STACK_BUFFER_OVERRUN"},
    ExceptionName{0x80000003, "EXCEPTION_BREAKPOINT"},
    ExceptionName{0xE06D7363, "C++ exception"},
};

std::string OrUnknown(const std::string& value)
{
    return value.empty() ? std::string{"unknown"} : value;
}
} // namespace

std::string DescribeExceptionCode(uint32_t code)
{
    for (const ExceptionName& entry : kExceptionNames)
    {
        if (entry.code == code)
        {
            return std::string{entry.name};
        }
    }
    return {};
}

std::string FormatCrashReport(const CrashReportInfo& info)
{
    std::string text;
    const auto line = [&text](std::string_view content)
    {
        text += content;
        text += '\n';
    };

    line("resourceLoader crash report");
    line("===========================");
    line("");
    line(fmt::format("Time:          {} UTC", OrUnknown(info.timestamp)));
    line(fmt::format("Loader:        {} ({})", OrUnknown(info.version),
                     OrUnknown(info.configuration)));
    line(fmt::format("Game:          {}", OrUnknown(info.gameBuild)));
    const std::string name = DescribeExceptionCode(info.exceptionCode);
    line(fmt::format("Exception:     {:#010x}{}", info.exceptionCode,
                     name.empty() ? std::string{} : " " + name));
    line(fmt::format("Address:       {}{}", OrUnknown(info.faultAddress),
                     info.inGameCall ? " (inside a game call made by the loader)" : ""));
    line("");
    line(fmt::format("Stage:         {}", OrUnknown(info.stage)));
    line(fmt::format("Resource:      {}", OrUnknown(info.resource)));
    line(fmt::format("File:          {}", OrUnknown(info.file)));
    if (!info.minidump.empty())
    {
        line(fmt::format("Minidump:      {}", info.minidump));
    }

    line("");
    line("Last actions, oldest first:");
    if (info.breadcrumbs.empty())
    {
        line("  (none)");
    }
    for (const std::string& breadcrumb : info.breadcrumbs)
    {
        line("  " + breadcrumb);
    }

    line("");
    line(fmt::format("Resources ({}):", info.resources.size()));
    for (const std::string& resource : info.resources)
    {
        line("  " + resource);
    }

    line("");
    line("What happens next:");
    if (!info.resource.empty())
    {
        line(
            fmt::format("  '{}' is quarantined on the next launch (resourceLoader/state.toml), and "
                        "everything else loads.",
                        info.resource));
        line("  Remove its name from state.toml once the resource is fixed.");
    }
    else
    {
        line("  The next launch starts in safe mode and registers nothing; the launch after that "
             "is normal.");
        line("  The last actions above usually name the resource to take out.");
    }
    line("  Set [loader] safe_mode = \"off\" in config.toml to skip this.");
    line("  When reporting the crash, attach this file and resourceLoader.log.");
    return text;
}
} // namespace spl
