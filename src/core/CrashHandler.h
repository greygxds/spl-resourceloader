#pragma once

#include <filesystem>
#include <functional>

#include "core/CrashReport.h"

namespace spl
{
/// Writes crash.txt (and optionally crash.dmp) when the game crashes in the loader's code, inside
/// a game call the loader made, or anywhere while the loader is changing game state (the game
/// then most likely tripped over what it was just given). It never handles a crash: the previous
/// filter, usually the game's or ScriptHookV's, always runs afterwards, and a crash that is
/// not ours is passed on untouched.
class CrashHandler
{
public:
    struct Settings
    {
        std::filesystem::path reportFile;
        std::filesystem::path dumpFile;
        bool writeMinidump = false;

        /// What only the application knows: the build, the resources and what streaming is
        /// doing. The handler fills in the exception itself. Runs inside the filter.
        std::function<CrashReportInfo()> describe;

        /// True while the loader is registering or changing game state, when a crash in any
        /// module is reported. Runs inside the filter.
        std::function<bool()> isBusy;

        /// Called after the report is written, with what it said.
        std::function<void(const CrashReportInfo&)> onCrash;
    };

    /// Chains in front of whatever filter is installed. A second call replaces the settings.
    static void Install(Settings settings);

    /// Puts our filter back in front when something replaced it since Install, keeping theirs
    /// chained behind it. Installed while the game starts, the game's own filter comes later.
    static void EnsureFirst();

    /// Puts the previous filter back, unless somebody chained in front of ours since; their
    /// filter then keeps calling ours, which no longer does anything.
    static void Uninstall();
};
} // namespace spl
