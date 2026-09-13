#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spl
{
/// Everything crash.txt says. Collected by the crash handler, formatted here so the text can
/// be tested without crashing anything.
struct CrashReportInfo
{
    uint32_t exceptionCode = 0;
    std::string faultAddress; ///< "resourceLoader.asi+0x1234" or "GTA5.exe+0x2A3C5B0"
    bool inGameCall = false;  ///< the fault happened inside a game call the loader made
    std::string timestamp;    ///< UTC, "2026-09-13 12:34:56"
    std::string version;      ///< Version::Describe()
    std::string configuration;
    std::string gameBuild; ///< GameBuild::ToString(), or empty before detection
    std::string stage;     ///< what streaming was doing
    std::string resource;
    std::string file;
    std::vector<std::string> breadcrumbs; ///< oldest first
    std::vector<std::string> resources;   ///< "name (state)", in load order
    std::string minidump;                 ///< where crash.dmp went, or empty
};

/// A readable name for the common exception codes ("EXCEPTION_ACCESS_VIOLATION"), or empty.
[[nodiscard]] std::string DescribeExceptionCode(uint32_t code);

/// The text of crash.txt: what failed, where, what the loader was doing and what it had loaded,
/// followed by what the user can do next.
[[nodiscard]] std::string FormatCrashReport(const CrashReportInfo& info);
} // namespace spl
