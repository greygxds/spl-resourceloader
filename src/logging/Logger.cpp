#include "logging/Logger.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/ansicolor_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "config/LoaderConfig.h"
#include "core/Version.h"
#include "logging/AsciiArt.h"
#include "platform/Win32.h"

namespace spl::logging
{
namespace
{
/// The log is truncated on every start, so the date is written once, on the second line.
constexpr std::string_view kFilePattern = "[%H:%M:%S.%e] [%n] [%l] %v";

/// The same in the console window: gray time, cyan channel, the level in its own color and the
/// message in the console's default. %^ and %$ delimit what the sink colors by level.
constexpr std::string_view kConsolePattern =
    "\x1b[90m[%H:%M:%S.%e]\x1b[0m \x1b[36m[%n]\x1b[0m %^[%l]%$ %v";

constexpr std::string_view kGray = "\x1b[90m";
constexpr std::string_view kGreen = "\x1b[32m";
constexpr std::string_view kYellow = "\x1b[33m";
constexpr std::string_view kRed = "\x1b[31m";
constexpr std::string_view kBrightRed = "\x1b[91;1m";
/// Two empty lines above and below the art, so it stands apart from the lines around it.
constexpr std::string_view kBannerPadding = "\n\n";
constexpr std::string_view kLogFileName = "resourceLoader.log"; ///< inside the data folder
constexpr bool kTruncateOnStart = true; ///< only the last run is ever of interest
/// Warnings and worse reach the disk straight away: a crash must not eat them.
constexpr spdlog::level::level_enum kFlushLevel = spdlog::level::warn;
constexpr std::size_t kChannelCount = 8;
constexpr std::size_t kBreadcrumbCapacity = 32;

struct BootstrapMessage
{
    Channel channel;
    spdlog::level::level_enum level;
    std::string text;
};

struct State
{
    std::mutex mutex;
    std::vector<spdlog::sink_ptr> sinks;
    std::array<std::shared_ptr<spdlog::logger>, kChannelCount> channels;
    std::shared_ptr<spdlog::logger> startup; ///< normal prefix, never filtered by level
    std::shared_ptr<spdlog::logger> discard; ///< used before Initialize()
    std::vector<BootstrapMessage> bootstrap;
    std::array<std::string, kBreadcrumbCapacity> breadcrumbs;
    std::size_t breadcrumbCount = 0; ///< total ever recorded; wraps into the array
    bool initialized = false;
    bool bannerWritten = false;
};

State& GetState()
{
    static State state;
    return state;
}

/// Each sink carries its own pattern, so a logger must never set one: spdlog would push it onto
/// every sink, and the file would get the console's color codes.
std::shared_ptr<spdlog::logger> MakeLogger(const State& state, std::string_view name,
                                           spdlog::level::level_enum level,
                                           spdlog::level::level_enum flushLevel)
{
    auto logger =
        std::make_shared<spdlog::logger>(std::string{name}, state.sinks.begin(), state.sinks.end());
    logger->set_level(level);
    logger->flush_on(flushLevel);
    return logger;
}

/// A logger with no sinks: every call is a cheap no-op. Returned by Get() before Initialize(),
/// so callers never have to check for null or for "is logging up yet".
std::shared_ptr<spdlog::logger> GetDiscardLogger(State& state)
{
    if (!state.discard)
    {
        state.discard = std::make_shared<spdlog::logger>("discard");
        state.discard->set_level(spdlog::level::off);
    }
    return state.discard;
}

/// Writes text to file, truncating or appending. Used for the version line, which must reach
/// the file without the per-line prefix that the sinks apply.
bool WriteRaw(const std::filesystem::path& file, std::string_view text, bool truncate)
{
    const std::ios::openmode mode = std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
    std::ofstream stream{file, mode};
    if (!stream)
    {
        return false;
    }
    stream << text;
    return stream.good();
}

/// "Started 2026-09-13 17:39:20 (UTC+02:00)", newline-terminated, in local time.
std::string StartedLine()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (::localtime_s(&local, &now) != 0)
    {
        return "Started at an unknown time\n";
    }
    // _mkgmtime reads the local fields as if they were UTC; the difference is the offset.
    std::tm fields = local;
    const auto offsetMinutes = static_cast<long long>(::_mkgmtime(&fields) - now) / 60;
    const long long magnitude = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
    return fmt::format("Started {:04}-{:02}-{:02} {:02}:{:02}:{:02} (UTC{}{:02}:{:02})\n",
                       local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                       local.tm_min, local.tm_sec, offsetMinutes < 0 ? '-' : '+', magnitude / 60,
                       magnitude % 60);
}

/// Lets the console window interpret the color codes. False on a console that cannot, which
/// then gets the plain pattern instead of escape sequences.
bool EnableConsoleColors()
{
    HANDLE const output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output == INVALID_HANDLE_VALUE || ::GetConsoleMode(output, &mode) == 0)
    {
        return false;
    }
    return ::SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

/// The console sink: colored when the window supports it, plain otherwise.
spdlog::sink_ptr MakeConsoleSink()
{
    auto sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
    if (!EnableConsoleColors())
    {
        sink->set_color_mode(spdlog::color_mode::never);
        sink->set_pattern(std::string{kFilePattern});
        return sink;
    }
    sink->set_color_mode(spdlog::color_mode::always);
    sink->set_color(spdlog::level::trace, kGray);
    sink->set_color(spdlog::level::debug, kGray);
    sink->set_color(spdlog::level::info, kGreen);
    sink->set_color(spdlog::level::warn, kYellow);
    sink->set_color(spdlog::level::err, kRed);
    sink->set_color(spdlog::level::critical, kBrightRed);
    sink->set_pattern(std::string{kConsolePattern});
    return sink;
}

/// Opens a console window and points stdout at it. Only ever called when the user asked for
/// it with loader.console, because GTA V has no console of its own.
bool AttachConsole()
{
    if (::GetConsoleWindow() != nullptr)
    {
        return true; // something already gave this process a console
    }
    if (::AllocConsole() == 0)
    {
        return false;
    }

    // The banner and the em dash in the version line are UTF-8.
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleTitleW(L"Singleplayer Resource Loader");

    FILE* stream = nullptr;
    if (::freopen_s(&stream, "CONOUT$", "w", stdout) != 0)
    {
        return false;
    }
    return true;
}
} // namespace

std::string_view ToString(Channel channel)
{
    using enum Channel;
    switch (channel)
    {
    case Core:
        return "core";
    case Config:
        return "config";
    case Resource:
        return "resource";
    case Manifest:
        return "manifest";
    case Streaming:
        return "streaming";
    case Rage:
        return "rage";
    case Hook:
        return "hook";
    case Mods:
        return "mods";
    }
    return "core";
}

spdlog::level::level_enum ToSpdlogLevel(config::LogLevel level)
{
    using enum config::LogLevel;
    switch (level)
    {
    case Trace:
        return spdlog::level::trace;
    case Debug:
        return spdlog::level::debug;
    case Info:
        return spdlog::level::info;
    case Warning:
        return spdlog::level::warn;
    case Error:
        return spdlog::level::err;
    case Critical:
        return spdlog::level::critical;
    case Off:
        return spdlog::level::off;
    }
    return spdlog::level::warn;
}

std::vector<std::string> SplitBannerLines(std::string_view text)
{
    if (text.starts_with("\r\n"))
    {
        text.remove_prefix(2);
    }
    else if (text.starts_with('\n'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }

    std::vector<std::string> lines;
    while (!text.empty())
    {
        const std::size_t breakPos = text.find('\n');
        std::string_view line = text.substr(0, breakPos);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (breakPos == std::string_view::npos)
        {
            break;
        }
        text.remove_prefix(breakPos + 1);
    }
    return lines;
}

bool Initialize(const config::LoggingSettings& settings, bool console,
                const std::filesystem::path& dataDir)
{
    State& state = GetState();
    std::vector<BootstrapMessage> pending;

    {
        const std::lock_guard lock{state.mutex};
        if (state.initialized)
        {
            return true; // ScriptMain can run again after a session reload
        }

        const spdlog::level::level_enum level = ToSpdlogLevel(settings.level);
        // Flush at warning, and on every line when the user asked for debug or trace: that is
        // a debugging session, and a line still in a buffer when the game crashes is the line
        // they needed. spdlog::flush_every() is deliberately not used, because its worker
        // thread would still be registered at DLL_PROCESS_DETACH.
        const spdlog::level::level_enum flushLevel = std::min(kFlushLevel, level);

        const std::string banner = BannerText();
        try
        {
            const std::filesystem::path logFile = dataDir / kLogFileName;
            std::error_code error;
            std::filesystem::create_directories(logFile.parent_path(), error);

            // The version line goes in first, written directly, because spdlog applies a sink's
            // pattern to every logger that shares it: a second logger with a "%v" pattern
            // would strip the prefix off the channel lines too. Writing it before the sink
            // exists is both simpler and exactly what "no prefix" means. The sink then always
            // appends, since truncation already happened here. The ASCII art is for the
            // console only: in the file it is just noise above the lines that matter.
            if (!WriteRaw(logFile, VersionLine() + StartedLine(), kTruncateOnStart))
            {
                return false;
            }
            state.bannerWritten = true;

            auto fileSink =
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.wstring(), false);
            fileSink->set_pattern(std::string{kFilePattern});
            state.sinks.push_back(std::move(fileSink));
        }
        catch (const std::exception&)
        {
            return false; // Get() keeps working and discards; the game is never disturbed
        }

        if (console && AttachConsole())
        {
            std::fputs(banner.c_str(), stdout);
            state.sinks.push_back(MakeConsoleSink());
        }

        for (std::size_t index = 0; index < kChannelCount; ++index)
        {
            const auto channel = static_cast<Channel>(index);
            state.channels.at(index) = MakeLogger(state, ToString(channel), level, flushLevel);
        }

        // Ignores the configured level on purpose: a user running at the default "warning"
        // must still see that the loader started and which build it is.
        state.startup =
            MakeLogger(state, ToString(Channel::Core), spdlog::level::trace, flushLevel);

        state.initialized = true;
        pending = std::exchange(state.bootstrap, {});
    }

    // Replayed after the version line, so the log reads in the order things happened.
    for (const BootstrapMessage& message : pending)
    {
        Get(message.channel)->log(message.level, message.text);
    }
    return true;
}

std::string BannerText()
{
    std::string text{kBannerPadding};
    for (const std::string& line : SplitBannerLines(kAsciiArt))
    {
        text += line;
        text += '\n';
    }
    text += kBannerPadding;
    text += VersionLine();
    return text;
}

std::string VersionLine()
{
    // The version is the first thing to ask a user for in a bug report, so it sits above
    // everything the log level can filter.
    return fmt::format("Singleplayer Resource Loader — {}\n", Version::Describe());
}

void LogStartup(std::string_view message)
{
    State& state = GetState();
    std::shared_ptr<spdlog::logger> startup;
    {
        const std::lock_guard lock{state.mutex};
        startup = state.startup;
    }
    if (startup)
    {
        startup->info(message);
        startup->flush();
    }
}

void KeepConsoleVisible()
{
    HWND const window = ::GetConsoleWindow();
    if (window == nullptr || ::IsWindowVisible(window) != 0)
    {
        return;
    }
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    SPL_LOG_DEBUG(Core, "The console window was hidden; shown again");
}

void LogCommandOutput(std::string_view message)
{
    LogStartup(message);
}

std::shared_ptr<spdlog::logger> Get(Channel channel)
{
    State& state = GetState();
    const std::lock_guard lock{state.mutex};

    const auto index = static_cast<std::size_t>(channel);
    if (!state.initialized || index >= state.channels.size() || !state.channels.at(index))
    {
        return GetDiscardLogger(state);
    }
    return state.channels.at(index);
}

void Bootstrap(Channel channel, spdlog::level::level_enum level, std::string message)
{
    State& state = GetState();
    std::shared_ptr<spdlog::logger> logger;
    {
        const std::lock_guard lock{state.mutex};
        if (!state.initialized)
        {
            state.bootstrap.push_back(
                BootstrapMessage{.channel = channel, .level = level, .text = std::move(message)});
            return;
        }
        logger = state.channels.at(static_cast<std::size_t>(channel));
    }
    logger->log(level, message); // logging is already up: no point deferring
}

void Breadcrumb(std::string_view action)
{
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    state.breadcrumbs.at(state.breadcrumbCount % kBreadcrumbCapacity) = action;
    ++state.breadcrumbCount;
}

namespace
{
std::vector<std::string> CopyBreadcrumbs(const State& state)
{
    const std::size_t count = std::min(state.breadcrumbCount, kBreadcrumbCapacity);
    const std::size_t oldest = state.breadcrumbCount <= kBreadcrumbCapacity
                                   ? 0
                                   : state.breadcrumbCount % kBreadcrumbCapacity;

    std::vector<std::string> result;
    result.reserve(count);
    for (std::size_t offset = 0; offset < count; ++offset)
    {
        result.push_back(state.breadcrumbs.at((oldest + offset) % kBreadcrumbCapacity));
    }
    return result;
}
} // namespace

std::vector<std::string> GetBreadcrumbs()
{
    State& state = GetState();
    const std::lock_guard lock{state.mutex};
    return CopyBreadcrumbs(state);
}

std::optional<std::vector<std::string>> TryGetBreadcrumbs()
{
    State& state = GetState();
    const std::unique_lock lock{state.mutex, std::try_to_lock};
    if (!lock.owns_lock())
    {
        return std::nullopt;
    }
    return CopyBreadcrumbs(state);
}

void Flush()
{
    State& state = GetState();
    // A crash can happen with the lock held; a log that misses its tail beats a hung game.
    const std::unique_lock lock{state.mutex, std::try_to_lock};
    if (!lock.owns_lock())
    {
        return;
    }
    for (const std::shared_ptr<spdlog::logger>& logger : state.channels)
    {
        if (logger)
        {
            logger->flush();
        }
    }
    if (state.startup)
    {
        state.startup->flush();
    }
}

void Shutdown()
{
    State& state = GetState();
    const std::lock_guard lock{state.mutex};

    // Only flushes. Never calls spdlog::shutdown() or joins a thread, because this runs from
    // DLL_PROCESS_DETACH where the loader lock is held.
    for (const std::shared_ptr<spdlog::logger>& logger : state.channels)
    {
        if (logger)
        {
            logger->flush();
        }
    }
    if (state.startup)
    {
        state.startup->flush();
    }

    state.channels = {};
    state.startup.reset();
    state.sinks.clear();
    state.bootstrap.clear();
    state.initialized = false;
    state.bannerWritten = false;
}
} // namespace spl::logging
