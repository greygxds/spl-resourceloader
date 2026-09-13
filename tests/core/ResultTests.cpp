#include <memory>
#include <string>
#include <utility>

#include <catch_amalgamated.hpp>

#include "core/Result.h"

using spl::Error;
using spl::ErrorCode;
using spl::MakeError;
using spl::Result;

TEST_CASE("Result: a value result carries the value", "[core]")
{
    const Result<int> result = 42;

    REQUIRE(result.HasValue());
    CHECK(static_cast<bool>(result));
    CHECK(result.GetValue() == 42);
}

TEST_CASE("Result: an error result carries code and message", "[core]")
{
    const Result<int> result = MakeError(ErrorCode::NotFound, "no hook named '{}'", "Test::Foo");

    REQUIRE_FALSE(result.HasValue());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.GetError().code == ErrorCode::NotFound);
    CHECK(result.GetMessage() == "no hook named 'Test::Foo'");
}

TEST_CASE("Result: a void result defaults to success", "[core]")
{
    const Result<void> success;
    const Result<void> failure = MakeError(ErrorCode::Io, "cannot write");

    CHECK(success.HasValue());
    REQUIRE_FALSE(failure.HasValue());
    CHECK(failure.GetError().code == ErrorCode::Io);
    CHECK(failure.GetMessage() == "cannot write");
}

TEST_CASE("Result: a move-only value survives the round trip", "[core]")
{
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);

    REQUIRE(result.HasValue());
    const std::unique_ptr<int> taken = std::move(result.GetValue());
    REQUIRE(taken != nullptr);
    CHECK(*taken == 7);
}

TEST_CASE("Result: every error code has a name", "[core]")
{
    CHECK(spl::ToString(ErrorCode::NotFound) == "not found");
    CHECK(spl::ToString(ErrorCode::Ambiguous) == "ambiguous");
    CHECK(spl::ToString(ErrorCode::Unavailable) == "unavailable");
    CHECK_FALSE(spl::ToString(ErrorCode::Unknown).empty());
}
