#pragma once

#include <string>
#include <string_view>

// CI writes core/BuildVersion.h before building. It is git-ignored, so a
// local tree has no such file and falls back to development values. This is the only place
// in the codebase that uses these macros; everything else uses spl::Version.
#if __has_include("core/BuildVersion.h")
#include "core/BuildVersion.h"
#else
#define SPL_BUILD_NUMBER 0
#define SPL_VERSION_TEXT "Development"
#define SPL_COMMIT_SHA "local"
#endif

namespace spl
{
struct Version
{
    static constexpr int kBuildNumber = SPL_BUILD_NUMBER;
    static constexpr std::string_view kText =
        SPL_VERSION_TEXT; // "Build 2026.09.12_57" or "Development"
    static constexpr std::string_view kCommit = SPL_COMMIT_SHA; // "a1b2c3d4" or "local"

    /// "Build 2026.09.12_57 (a1b2c3d4)", or "Development (local)" for a local build.
    [[nodiscard]] static std::string Describe();

    /// Build configuration this binary was compiled in: "Debug", "Dev" or "Release".
    [[nodiscard]] static constexpr std::string_view Configuration()
    {
#if defined(_DEBUG)
        return "Debug";
#elif SPL_DEV_TOOLS
        return "Dev";
#else
        return "Release";
#endif
    }
};
} // namespace spl
