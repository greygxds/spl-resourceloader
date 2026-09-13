#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <spdlog/spdlog.h>

#include "config/LoaderConfig.h"
#include "logging/Logger.h"

using spl::config::LogLevel;
using spl::logging::Channel;

TEST_CASE("Logger: every channel has its documented name", "[logging]")
{
    REQUIRE(spl::logging::ToString(Channel::Core) == "core");
    REQUIRE(spl::logging::ToString(Channel::Config) == "config");
    REQUIRE(spl::logging::ToString(Channel::Resource) == "resource");
    REQUIRE(spl::logging::ToString(Channel::Manifest) == "manifest");
    REQUIRE(spl::logging::ToString(Channel::Streaming) == "streaming");
    REQUIRE(spl::logging::ToString(Channel::Rage) == "rage");
    REQUIRE(spl::logging::ToString(Channel::Hook) == "hook");
    REQUIRE(spl::logging::ToString(Channel::Mods) == "mods");
}

TEST_CASE("Logger: our levels map onto spdlog's", "[logging]")
{
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Trace) == spdlog::level::trace);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Debug) == spdlog::level::debug);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Info) == spdlog::level::info);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Warning) == spdlog::level::warn);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Error) == spdlog::level::err);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Critical) == spdlog::level::critical);
    REQUIRE(spl::logging::ToSpdlogLevel(LogLevel::Off) == spdlog::level::off);
}

TEST_CASE("Logger: Get works before Initialize and discards", "[logging]")
{
    const auto logger = spl::logging::Get(Channel::Core);

    REQUIRE(logger != nullptr);
    REQUIRE_NOTHROW(logger->info("this goes nowhere"));
}

TEST_CASE("Logger: breadcrumbs keep the most recent actions in order", "[logging]")
{
    for (int index = 0; index < 40; ++index)
    {
        spl::logging::Breadcrumb("action " + std::to_string(index));
    }

    const std::vector<std::string> crumbs = spl::logging::GetBreadcrumbs();

    REQUIRE(crumbs.size() == 32);
    REQUIRE(crumbs.front() == "action 8");
    REQUIRE(crumbs.back() == "action 39");
}
