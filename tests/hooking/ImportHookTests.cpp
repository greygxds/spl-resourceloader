#include <atomic>

#include <catch_amalgamated.hpp>

#include "hooking/ImportHook.h"
#include "platform/Win32.h"

using spl::hooking::ImportHook;

namespace
{
using GetCurrentProcessIdFn = DWORD(WINAPI*)();

std::atomic<int> s_calls{0};
GetCurrentProcessIdFn s_original = nullptr;

DWORD WINAPI GetCurrentProcessIdDetour()
{
    ++s_calls;
    return s_original();
}

uintptr_t MainModuleBase()
{
    return reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
}
} // namespace

TEST_CASE("ImportHook: redirects the executable's import and restores it", "[hooking]")
{
    const DWORD expected = ::GetCurrentProcessId();
    s_calls = 0;
    {
        spl::Result<ImportHook> hook =
            ImportHook::Install(MainModuleBase(), "KERNEL32.dll", "GetCurrentProcessId",
                                reinterpret_cast<void*>(&GetCurrentProcessIdDetour));
        REQUIRE(hook);
        REQUIRE(hook.GetValue().IsInstalled());
        s_original = reinterpret_cast<GetCurrentProcessIdFn>(hook.GetValue().GetOriginal());

        CHECK(::GetCurrentProcessId() == expected);
        CHECK(s_calls >= 1);

        hook.GetValue().Restore();
        CHECK_FALSE(hook.GetValue().IsInstalled());
    }
    const int callsAfterRestore = s_calls;
    CHECK(::GetCurrentProcessId() == expected);
    CHECK(s_calls == callsAfterRestore);
}

TEST_CASE("ImportHook: a function the module does not import is refused", "[hooking]")
{
    const spl::Result<ImportHook> hook =
        ImportHook::Install(MainModuleBase(), "KERNEL32.dll", "Beep",
                            reinterpret_cast<void*>(&GetCurrentProcessIdDetour));
    REQUIRE_FALSE(hook);
}
