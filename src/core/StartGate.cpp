#include "core/StartGate.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>

#include "core/Application.h"
#include "hooking/ImportHook.h"
#include "platform/Win32.h"

namespace spl
{
namespace
{
constexpr const char* kKernel32 = "KERNEL32.dll";

/// Each attempt before the code is decrypted scans the image once. The C runtime makes a
/// handful of these calls on startup; past this many the game is doing something unexpected.
constexpr int kMaxAttempts = 32;

using GetCommandLineAFn = LPSTR(WINAPI*)();
using GetStartupInfoWFn = void(WINAPI*)(LPSTARTUPINFOW);

hooking::ImportHook g_commandLineHook;
hooking::ImportHook g_startupInfoHook;
GetCommandLineAFn g_getCommandLineA = nullptr;
GetStartupInfoWFn g_getStartupInfoW = nullptr;

std::atomic<bool> g_closed{false};
std::atomic<int> g_attempts{0};
std::mutex g_gateMutex;
thread_local bool t_insideGate = false;

void Close()
{
    g_closed = true;
    g_commandLineHook.Restore();
    g_startupInfoHook.Restore();
}

/// Asks the application to start. Reentrant calls (our own start-up reads the command line
/// too) and calls from other threads while an attempt runs just pass through.
void TryOpen()
{
    if (g_closed.load(std::memory_order_relaxed) || t_insideGate)
    {
        return;
    }
    const std::unique_lock lock{g_gateMutex, std::try_to_lock};
    if (!lock.owns_lock() || g_closed)
    {
        return;
    }

    t_insideGate = true;
    EarlyStart result = EarlyStart::Declined;
    try
    {
        result = Application::Instance().TryStartEarly();
    }
    catch (const std::exception&)
    {
        result = EarlyStart::Declined; // ScriptMain still gets its turn
    }
    t_insideGate = false;

    if (result == EarlyStart::NotReady && ++g_attempts < kMaxAttempts)
    {
        return;
    }
    Close();
}

LPSTR WINAPI GetCommandLineADetour()
{
    TryOpen();
    return g_getCommandLineA();
}

void WINAPI GetStartupInfoWDetour(LPSTARTUPINFOW startupInfo)
{
    TryOpen();
    g_getStartupInfoW(startupInfo);
}
} // namespace

void ArmStartGate()
{
    // The detours may run the moment a slot is written, so they get the real functions first.
    HMODULE const kernel32 = ::GetModuleHandleA(kKernel32);
    g_getCommandLineA =
        reinterpret_cast<GetCommandLineAFn>(::GetProcAddress(kernel32, "GetCommandLineA"));
    g_getStartupInfoW =
        reinterpret_cast<GetStartupInfoWFn>(::GetProcAddress(kernel32, "GetStartupInfoW"));

    const auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    if (Result<hooking::ImportHook> hook = hooking::ImportHook::Install(
            base, kKernel32, "GetCommandLineA", reinterpret_cast<void*>(&GetCommandLineADetour)))
    {
        g_getCommandLineA = reinterpret_cast<GetCommandLineAFn>(hook.GetValue().GetOriginal());
        g_commandLineHook = std::move(hook.GetValue());
    }
    if (Result<hooking::ImportHook> hook = hooking::ImportHook::Install(
            base, kKernel32, "GetStartupInfoW", reinterpret_cast<void*>(&GetStartupInfoWDetour)))
    {
        g_getStartupInfoW = reinterpret_cast<GetStartupInfoWFn>(hook.GetValue().GetOriginal());
        g_startupInfoHook = std::move(hook.GetValue());
    }
}

void DisarmStartGate()
{
    if (!g_closed.exchange(true))
    {
        g_commandLineHook.Restore();
        g_startupInfoHook.Restore();
    }
}
} // namespace spl
