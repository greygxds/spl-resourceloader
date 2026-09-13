#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "core/Version.h"
#include "logging/AsciiArt.h"
#include "logging/Logger.h"
#include "tests/TempTree.h"

using spl::config::LoggingSettings;
using spl::config::LogLevel;
using spl::logging::Channel;

namespace
{
/// Drives the real logger into a temp folder and hands back what landed in the file.
/// Logging state is process-wide, so each fixture initializes and shuts down in turn.
class LogFixture
{
public:
    explicit LogFixture(LogLevel level)
    {
        LoggingSettings settings;
        settings.level = level;
        // false: never open a console window from a test.
        REQUIRE(spl::logging::Initialize(settings, false, m_dir.Path()));
    }

    ~LogFixture()
    {
        spl::logging::Shutdown();
    }

    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;

    [[nodiscard]] std::string ReadLog() const
    {
        spl::logging::Get(Channel::Core)->flush();
        return m_dir.ReadFile("resourceLoader.log");
    }

private:
    spl::tests::TempDir m_dir;
};

bool Contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}
} // namespace

TEST_CASE("Logger: the log opens with the version and no ASCII art", "[logging]")
{
    const LogFixture fixture{LogLevel::Warning};

    const std::string log = fixture.ReadLog();

    // The version line is at the very top and unprefixed; the art is console-only.
    INFO("log head: [" << log.substr(0, 120) << "]");
    REQUIRE(log.starts_with(spl::logging::VersionLine()));
    REQUIRE(Contains(log, spl::Version::kText));
    REQUIRE_FALSE(Contains(log, spl::logging::SplitBannerLines(spl::logging::kAsciiArt).front()));
}

TEST_CASE("Logger: startup lines survive the warning level", "[logging]")
{
    const LogFixture fixture{LogLevel::Warning};
    spl::logging::LogStartup("Loader initialized (configuration: Debug, log level: warning)");

    const std::string log = fixture.ReadLog();

    REQUIRE(Contains(log, "[core] [info] Loader initialized"));
}

TEST_CASE("Logger: the level filters ordinary channel lines", "[logging]")
{
    const LogFixture fixture{LogLevel::Warning};
    SPL_LOG_INFO(Streaming, "this info line is below the level");
    SPL_LOG_WARNING(Streaming, "this warning line is at the level");

    const std::string log = fixture.ReadLog();

    REQUIRE_FALSE(Contains(log, "below the level"));
    REQUIRE(Contains(log, "[streaming] [warning] this warning line is at the level"));
}

TEST_CASE("Logger: debug level lets every channel through", "[logging]")
{
    const LogFixture fixture{LogLevel::Debug};
    SPL_LOG_DEBUG(Config, "config detail");
    SPL_LOG_DEBUG(Resource, "resource detail");
    SPL_LOG_TRACE(Rage, "trace is still below debug");

    const std::string log = fixture.ReadLog();

    REQUIRE(Contains(log, "[config] [debug] config detail"));
    REQUIRE(Contains(log, "[resource] [debug] resource detail"));
    REQUIRE_FALSE(Contains(log, "trace is still below debug"));
}

TEST_CASE("Logger: level off still writes the version and the startup lines", "[logging]")
{
    const LogFixture fixture{LogLevel::Off};
    SPL_LOG_ERROR(Core, "not even errors are logged at level off");
    spl::logging::LogStartup("Loader initialized");

    const std::string log = fixture.ReadLog();

    REQUIRE(Contains(log, "Singleplayer Resource Loader"));
    REQUIRE(Contains(log, "Loader initialized"));
    REQUIRE_FALSE(Contains(log, "not even errors"));
}

TEST_CASE("Logger: messages logged before Initialize are replayed", "[logging]")
{
    spl::logging::Bootstrap(Channel::Config, spdlog::level::warn, "early config warning");

    const LogFixture fixture{LogLevel::Warning};

    const std::string log = fixture.ReadLog();
    REQUIRE(Contains(log, "[config] [warning] early config warning"));
    // Replayed after the version line, so the log still reads top to bottom.
    REQUIRE(log.find("Singleplayer Resource Loader") < log.find("early config warning"));
}

TEST_CASE("Logger: the start date is on the second line, and lines carry only the time",
          "[logging]")
{
    const LogFixture fixture{LogLevel::Warning};
    SPL_LOG_WARNING(Core, "a line with a time");

    const std::string log = fixture.ReadLog();

    const std::size_t secondLine = spl::logging::VersionLine().size();
    REQUIRE(log.compare(secondLine, 8, "Started ") == 0);
    REQUIRE(Contains(log.substr(secondLine, log.find('\n', secondLine) - secondLine), "(UTC"));

    const std::size_t line = log.find("[core] [warning] a line with a time");
    REQUIRE(line != std::string::npos);
    // "[17:39:32.987] " sits in front of the channel: 15 characters, no date.
    const std::size_t lineStart = log.rfind('\n', line) + 1;
    CHECK(line - lineStart == 15);
    CHECK(log[lineStart] == '[');
    CHECK(log[lineStart + 3] == ':');
    CHECK_FALSE(Contains(log, "\x1b[")); // the console's colors never reach the file
}
