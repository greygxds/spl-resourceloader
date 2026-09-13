#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "config/LoaderConfig.h"

namespace spl::logging
{
/// One channel per subsystem. The channel name is what appears
/// between the timestamp and the level in every log line.
enum class Channel
{
    Core,
    Config,
    Resource,
    Manifest,
    Streaming,
    Rage,
    Hook,
    Mods ///< user-installed .rpf mods
};

[[nodiscard]] std::string_view ToString(Channel channel);

/// Creates the sinks from settings, writes the version line and the start date and time (with the
/// ASCII art only on the console), then replays whatever Bootstrap() collected before this point.
/// Lines carry the time of day only; the console colors their parts. The log file always
/// lives in dataDir and is truncated at startup; console opens the live output window
/// (loader.console). Returns false when no sink could be opened; Get() still returns a usable
/// logger that discards everything, so callers never have to null-check.
bool Initialize(const config::LoggingSettings& settings, bool console,
                const std::filesystem::path& dataDir);

/// The ASCII art plus the version line, newline-terminated. Initialize() writes this to the
/// console when one is open, before any sink exists.
[[nodiscard]] std::string BannerText();

/// "Singleplayer Resource Loader — <version>", newline-terminated. Initialize() writes this,
/// without the art, to the top of the log file.
[[nodiscard]] std::string VersionLine();

/// Writes a startup line that the configured level must never filter out, such as "Loader
/// initialized". It carries the normal prefix on the core channel.
void LogStartup(std::string_view message);

/// Writes what a console command printed. Like LogStartup, the configured level never filters
/// it out: the user asked for it.
void LogCommandOutput(std::string_view message);

/// Shows the console window again when something hid it, without taking focus from the game.
/// GTA V hides a console it finds while it starts, which is where early init opens ours. Cheap
/// enough for every tick; does nothing without a console.
void KeepConsoleVisible();

/// Flushes every sink without dropping anything. Gives up rather than wait when another thread
/// holds the logger, because the crash handler calls it.
void Flush();

/// Flushes and drops every logger. Safe to call from DLL_PROCESS_DETACH: it only flushes,
/// and never joins a thread.
void Shutdown();

/// The logger for one channel. Never null.
[[nodiscard]] std::shared_ptr<spdlog::logger> Get(Channel channel);

/// Records a message emitted before Initialize(), to be replayed into the log afterwards.
/// Anything logged this early would otherwise be lost, including config diagnostics.
void Bootstrap(Channel channel, spdlog::level::level_enum level, std::string message);

/// Records the last few actions for a crash report to dump. Costs nothing when
/// nothing crashes, and the ring buffer never grows.
void Breadcrumb(std::string_view action);

/// The recorded breadcrumbs, oldest first.
[[nodiscard]] std::vector<std::string> GetBreadcrumbs();

/// The same, for the crash handler: std::nullopt instead of waiting when the lock is held.
[[nodiscard]] std::optional<std::vector<std::string>> TryGetBreadcrumbs();

/// Strips the single leading and trailing newline of the banner text and splits it into
/// lines, tolerating CRLF. Free-standing so it can be unit-tested.
[[nodiscard]] std::vector<std::string> SplitBannerLines(std::string_view text);

/// Maps our level enum to spdlog's.
[[nodiscard]] spdlog::level::level_enum ToSpdlogLevel(config::LogLevel level);
} // namespace spl::logging

// Convenience wrappers. The channel is written without its enum prefix: SPL_LOG_INFO(Config, ...).
#define SPL_LOG_TRACE(channel, ...)                                                                \
    ::spl::logging::Get(::spl::logging::Channel::channel)->trace(__VA_ARGS__)
#define SPL_LOG_DEBUG(channel, ...)                                                                \
    ::spl::logging::Get(::spl::logging::Channel::channel)->debug(__VA_ARGS__)
#define SPL_LOG_INFO(channel, ...)                                                                 \
    ::spl::logging::Get(::spl::logging::Channel::channel)->info(__VA_ARGS__)
#define SPL_LOG_WARNING(channel, ...)                                                              \
    ::spl::logging::Get(::spl::logging::Channel::channel)->warn(__VA_ARGS__)
#define SPL_LOG_ERROR(channel, ...)                                                                \
    ::spl::logging::Get(::spl::logging::Channel::channel)->error(__VA_ARGS__)
#define SPL_LOG_CRITICAL(channel, ...)                                                             \
    ::spl::logging::Get(::spl::logging::Channel::channel)->critical(__VA_ARGS__)
