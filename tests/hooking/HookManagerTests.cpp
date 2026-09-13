#include <cstdint>
#include <string>

#include <catch_amalgamated.hpp>

#include "hooking/HookManager.h"

using namespace spl::hooking;
using spl::ErrorCode;

namespace
{
using AddOneFn = int (*)(int);

/// The function under test. It must not be inlined, and it has to be long enough for
/// MinHook to place a jump in it, which is what the volatile scratch value buys.
int __declspec(noinline) AddOne(int value)
{
    volatile int scratch = value;
    scratch += 1;
    return scratch;
}

AddOneFn s_addOneOriginal = nullptr;

/// Calls go through a volatile pointer, so the optimizer cannot fold AddOne(1) into 2 and
/// the test really observes the patched code.
AddOneFn volatile s_addOneEntry = &AddOne;

int AddOneDetour(int value)
{
    return s_addOneOriginal(value) * 10;
}

/// The manager is a process-wide singleton, so every test starts from a clean slate.
class HookScope
{
public:
    HookScope()
    {
        REQUIRE(HookManager::Instance().Initialize().HasValue());
    }

    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

    ~HookScope()
    {
        HookManager::Instance().Shutdown();
        s_addOneOriginal = nullptr;
    }
};

[[nodiscard]] uintptr_t AddOneAddress()
{
    return reinterpret_cast<uintptr_t>(static_cast<AddOneFn>(&AddOne));
}
} // namespace

TEST_CASE("HookManager: Initialize is idempotent", "[hook]")
{
    HookScope scope;

    CHECK(HookManager::Instance().IsInitialized());
    CHECK(HookManager::Instance().Initialize().HasValue());
}

TEST_CASE("HookManager: an enabled hook redirects the call", "[hook]")
{
    HookScope scope;
    HookManager& manager = HookManager::Instance();

    REQUIRE(
        manager.Create<AddOneFn>("Test::AddOne", AddOneAddress(), &AddOneDetour, &s_addOneOriginal)
            .HasValue());
    CHECK(manager.FindStatus("Test::AddOne") == HookStatus::Created);
    CHECK(s_addOneEntry(1) == 2); // created but not enabled: the original still runs

    REQUIRE(manager.Enable("Test::AddOne").HasValue());
    CHECK(manager.FindStatus("Test::AddOne") == HookStatus::Enabled);
    CHECK(s_addOneEntry(1) == 20);
    CHECK(s_addOneOriginal(1) == 2); // the trampoline reaches the untouched original

    manager.DisableAll();
    CHECK(manager.FindStatus("Test::AddOne") == HookStatus::Created);
    CHECK(s_addOneEntry(1) == 2);
}

TEST_CASE("HookManager: EnableAll enables every created hook", "[hook]")
{
    HookScope scope;
    HookManager& manager = HookManager::Instance();

    REQUIRE(
        manager.Create<AddOneFn>("Test::AddOne", AddOneAddress(), &AddOneDetour, &s_addOneOriginal)
            .HasValue());
    REQUIRE(manager.EnableAll().HasValue());

    CHECK(manager.FindStatus("Test::AddOne") == HookStatus::Enabled);
    CHECK(s_addOneEntry(2) == 30);
}

TEST_CASE("HookManager: Shutdown restores the original code", "[hook]")
{
    {
        HookScope scope;
        HookManager& manager = HookManager::Instance();
        REQUIRE(
            manager
                .Create<AddOneFn>("Test::AddOne", AddOneAddress(), &AddOneDetour, &s_addOneOriginal)
                .HasValue());
        REQUIRE(manager.Enable("Test::AddOne").HasValue());
        CHECK(s_addOneEntry(1) == 20);
    }
    CHECK(s_addOneEntry(1) == 2);
    CHECK(HookManager::Instance().GetHooks().empty());
}

TEST_CASE("HookManager: the same name cannot be hooked twice", "[hook]")
{
    HookScope scope;
    HookManager& manager = HookManager::Instance();

    REQUIRE(
        manager.Create<AddOneFn>("Test::AddOne", AddOneAddress(), &AddOneDetour, &s_addOneOriginal)
            .HasValue());

    const spl::Result<void> again =
        manager.Create<AddOneFn>("Test::AddOne", AddOneAddress(), &AddOneDetour, &s_addOneOriginal);
    REQUIRE_FALSE(again.HasValue());
    CHECK(again.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("HookManager: a hook without a target is rejected", "[hook]")
{
    HookScope scope;
    HookManager& manager = HookManager::Instance();

    const spl::Result<void> created =
        manager.Create<AddOneFn>("Test::NoTarget", 0, &AddOneDetour, &s_addOneOriginal);
    REQUIRE_FALSE(created.HasValue());
    CHECK(created.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("HookManager: enabling an unknown hook is a not-found error", "[hook]")
{
    HookScope scope;

    const spl::Result<void> enabled = HookManager::Instance().Enable("Test::Unknown");
    REQUIRE_FALSE(enabled.HasValue());
    CHECK(enabled.GetError().code == ErrorCode::NotFound);
    CHECK_FALSE(HookManager::Instance().FindStatus("Test::Unknown").has_value());
}

TEST_CASE("HookManager: creating a hook before Initialize is unavailable", "[hook]")
{
    HookManager& manager = HookManager::Instance();
    REQUIRE_FALSE(manager.IsInitialized()); // no HookScope in this test on purpose

    const spl::Result<void> created = manager.Create<AddOneFn>("Test::TooEarly", AddOneAddress(),
                                                               &AddOneDetour, &s_addOneOriginal);
    REQUIRE_FALSE(created.HasValue());
    CHECK(created.GetError().code == ErrorCode::Unavailable);
}
