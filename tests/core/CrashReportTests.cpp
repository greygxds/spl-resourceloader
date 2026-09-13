#include <string>

#include <catch_amalgamated.hpp>

#include "core/CrashReport.h"

using spl::CrashReportInfo;
using spl::FormatCrashReport;

TEST_CASE("CrashReport: names the exception, the culprit and what happens next", "[core]")
{
    const CrashReportInfo info{
        .exceptionCode = 0xC0000005,
        .faultAddress = "GTA5.exe+0x2a3c5b0",
        .inGameCall = true,
        .timestamp = "2026-09-13 12:00:00",
        .version = "Build 2026.09.13_60 (a1b2c3d4)",
        .configuration = "Release",
        .gameBuild = "1.0.3411.0 (Legacy, Steam, GTA5.exe)",
        .stage = "early registration",
        .resource = "bad_map",
        .file = "broken.ydr",
        .breadcrumbs = {"mounting the resources folder", "registering 'broken.ydr' from 'bad_map'"},
        .resources = {"bad_map (scanned)", "good (scanned)"}};

    const std::string text = FormatCrashReport(info);

    CHECK(text.find("0xc0000005 EXCEPTION_ACCESS_VIOLATION") != std::string::npos);
    CHECK(text.find("GTA5.exe+0x2a3c5b0 (inside a game call") != std::string::npos);
    CHECK(text.find("Build 2026.09.13_60") != std::string::npos);
    CHECK(text.find("1.0.3411.0") != std::string::npos);
    CHECK(text.find("registering 'broken.ydr' from 'bad_map'") != std::string::npos);
    CHECK(text.find("Resources (2):") != std::string::npos);
    CHECK(text.find("'bad_map' is quarantined") != std::string::npos);
}

TEST_CASE("CrashReport: an unknown culprit explains safe mode", "[core]")
{
    const std::string text = FormatCrashReport(CrashReportInfo{.exceptionCode = 0x12345678});

    CHECK(text.find("0x12345678\n") != std::string::npos);
    CHECK(text.find("Resource:      unknown") != std::string::npos);
    CHECK(text.find("safe mode") != std::string::npos);
    CHECK(text.find("(none)") != std::string::npos);
}

TEST_CASE("CrashReport: known exception codes have names", "[core]")
{
    CHECK(spl::DescribeExceptionCode(0xC00000FD) == "EXCEPTION_STACK_OVERFLOW");
    CHECK(spl::DescribeExceptionCode(1).empty());
}
