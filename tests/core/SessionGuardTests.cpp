#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "core/SessionGuard.h"
#include "tests/TempTree.h"

using spl::CrashMarker;
using spl::SessionGuard;
using spl::SessionStartup;
using spl::config::SafeMode;

namespace
{
SessionGuard MakeGuard(const spl::tests::TempDir& dir)
{
    return SessionGuard{dir.Path() / "running.marker", dir.Path() / "state.toml"};
}
} // namespace

TEST_CASE("SessionGuard: a clean previous session starts normally", "[core]")
{
    const SessionStartup startup = SessionGuard::Decide(std::nullopt, SafeMode::Auto, {"old"});

    CHECK_FALSE(startup.previousCrash);
    CHECK_FALSE(startup.safeMode);
    CHECK(startup.newlyQuarantined.empty());
    CHECK(startup.quarantined == std::vector<std::string>{"old"});
}

TEST_CASE("SessionGuard: a crash with no culprit starts in safe mode", "[core]")
{
    const SessionStartup startup =
        SessionGuard::Decide(CrashMarker{.stage = "early registration"}, SafeMode::Auto, {});

    REQUIRE(startup.previousCrash);
    CHECK(startup.safeMode);
    CHECK(startup.quarantined.empty());
}

TEST_CASE("SessionGuard: a crash that names a resource quarantines only it", "[core]")
{
    const SessionStartup startup = SessionGuard::Decide(
        CrashMarker{.stage = "late registration", .resource = "bad_map", .file = "x.ymap"},
        SafeMode::Auto, {"Other"});

    CHECK_FALSE(startup.safeMode);
    CHECK(startup.newlyQuarantined == "bad_map");
    CHECK(startup.quarantined == std::vector<std::string>{"Other", "bad_map"});
}

TEST_CASE("SessionGuard: an already quarantined culprit is not added twice", "[core]")
{
    const SessionStartup startup =
        SessionGuard::Decide(CrashMarker{.resource = "BAD_MAP"}, SafeMode::Auto, {"bad_map"});

    CHECK(startup.newlyQuarantined.empty());
    CHECK(startup.quarantined.size() == 1);
}

TEST_CASE("SessionGuard: safe_mode off reports the crash and changes nothing", "[core]")
{
    const SessionStartup startup =
        SessionGuard::Decide(CrashMarker{.resource = "bad_map"}, SafeMode::Off, {});

    CHECK(startup.previousCrash);
    CHECK_FALSE(startup.safeMode);
    CHECK(startup.quarantined.empty());
}

TEST_CASE("SessionGuard: the marker round-trips and tolerates junk", "[core]")
{
    const CrashMarker marker{.stage = "data files", .resource = "a\nb", .file = "c.ytyp"};
    const CrashMarker parsed = SessionGuard::ParseMarker(SessionGuard::FormatMarker(marker));

    CHECK(parsed.stage == "data files");
    CHECK(parsed.resource == "a b");
    CHECK(parsed.file == "c.ytyp");
    CHECK(SessionGuard::ParseMarker("") == CrashMarker{});
    CHECK(SessionGuard::ParseMarker("garbage\r\nresource= x \r\n").resource == "x");
}

TEST_CASE("SessionGuard: state.toml round-trips and rejects broken files", "[core]")
{
    const auto parsed = SessionGuard::ParseState(SessionGuard::FormatState({"one", "[two]"}));
    REQUIRE(parsed);
    CHECK(parsed.GetValue() == std::vector<std::string>{"one", "[two]"});

    const auto broken = SessionGuard::ParseState("[resources\n");
    REQUIRE_FALSE(broken);
    CHECK(broken.GetError().message.find("state.toml") != std::string::npos);

    const auto deduped =
        SessionGuard::ParseState("[resources]\nquarantined = [\" a \", \"A\", 3, \"\"]\n");
    REQUIRE(deduped);
    CHECK(deduped.GetValue() == std::vector<std::string>{"a"});
}

TEST_CASE("SessionGuard: a marker left behind is consumed by the next start", "[core]")
{
    const spl::tests::TempDir dir;
    {
        SessionGuard crashed = MakeGuard(dir);
        static_cast<void>(crashed.Begin(SafeMode::Auto));
        crashed.MarkBusy("early registration");
        crashed.RecordCrash(CrashMarker{.stage = "early registration", .resource = "bad_map"});
        // no MarkIdle(): the process died
    }

    SessionGuard next = MakeGuard(dir);
    const SessionStartup startup = next.Begin(SafeMode::Auto);
    CHECK(startup.previousCrash);
    CHECK(startup.newlyQuarantined == "bad_map");
    CHECK_FALSE(std::filesystem::exists(dir.Path() / "running.marker"));
    CHECK(dir.ReadFile("state.toml").find("bad_map") != std::string::npos);

    SessionGuard third = MakeGuard(dir);
    const SessionStartup after = third.Begin(SafeMode::Auto);
    CHECK_FALSE(after.previousCrash);
    CHECK(after.quarantined == std::vector<std::string>{"bad_map"});
}

TEST_CASE("SessionGuard: an idle session leaves no marker", "[core]")
{
    const spl::tests::TempDir dir;
    SessionGuard guard = MakeGuard(dir);
    static_cast<void>(guard.Begin(SafeMode::Auto));

    guard.MarkBusy("early registration");
    CHECK(std::filesystem::exists(dir.Path() / "running.marker"));
    guard.MarkIdle();
    CHECK_FALSE(std::filesystem::exists(dir.Path() / "running.marker"));

    guard.Quarantine("late");
    guard.Quarantine("LATE");
    SessionGuard next = MakeGuard(dir);
    CHECK(next.Begin(SafeMode::Auto).quarantined == std::vector<std::string>{"late"});
}

TEST_CASE("SessionGuard: state.toml is replaced whole, with no temporary left behind", "[core]")
{
    const spl::tests::TempDir dir;
    SessionGuard guard = MakeGuard(dir);
    static_cast<void>(guard.Begin(SafeMode::Auto));

    guard.Quarantine("first");
    guard.Quarantine("second"); // renamed over an existing state.toml

    CHECK_FALSE(std::filesystem::exists(dir.Path() / "state.toml.tmp"));
    SessionGuard next = MakeGuard(dir);
    CHECK(next.Begin(SafeMode::Auto).quarantined == std::vector<std::string>{"first", "second"});
}
