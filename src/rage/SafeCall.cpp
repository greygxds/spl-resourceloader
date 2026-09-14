#include "rage/SafeCall.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "memory/Module.h"
#include "platform/Win32.h"

namespace spl::rage
{
namespace
{
std::atomic<bool> g_faulted{false}; // set by whichever thread faults, read by the game thread
thread_local int g_gameCallDepth = 0;

/// The module an address belongs to, or nullptr when it belongs to none.
[[nodiscard]] HMODULE FindOwningModule(const void* address)
{
    HMODULE handle = nullptr;
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (::GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &handle) == FALSE)
    {
        return nullptr;
    }
    return handle;
}

/// This file's own module, which is the ASI. Taken from an address inside it, so no handle
/// has to be passed down from DllMain.
[[nodiscard]] HMODULE OwnModule()
{
    static HMODULE handle = FindOwningModule(reinterpret_cast<const void*>(&FindOwningModule));
    return handle;
}

/// A fault in game code or in ours is a layout mistake we can report. A fault anywhere else
/// belongs to whoever raised it, and swallowing it would hide someone else's bug.
[[nodiscard]] bool IsKnownCode(const void* address)
{
    const HMODULE owner = FindOwningModule(address);
    return owner != nullptr && (owner == ::GetModuleHandleW(nullptr) || owner == OwnModule());
}

[[nodiscard]] std::string DescribeAddress(const void* address)
{
    const auto value = reinterpret_cast<uintptr_t>(address);
    const memory::Module main = memory::Module::Main();
    if (main.Contains(value))
    {
        return fmt::format("{}+{:#x}", main.GetFileName(), value - main.GetBase());
    }
    return fmt::format("{:#x}", value);
}

/// Not inlined: the frame that owns the __except must stay free of anything with a
/// destructor, and this builds strings.
__declspec(noinline) int FaultFilter(const char* what, const EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !IsKnownCode(record.ExceptionAddress))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SPL_LOG_ERROR(
        Rage, "Game call '{}' faulted with {:#x} at {}: no further game calls will be made", what,
        static_cast<uint32_t>(record.ExceptionCode), DescribeAddress(record.ExceptionAddress));
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

namespace detail
{
bool InvokeGuarded(const char* what, void (*thunk)(void*), void* context)
{
    // Read by the crash handler, which runs before any unwinding: anything this call lets
    // through is still attributed to it.
    ++g_gameCallDepth;
    bool completed = false;
    __try
    {
        __try
        {
            thunk(context);
            completed = true;
        }
        __except (FaultFilter(what, GetExceptionInformation()))
        {
            g_faulted.store(true);
        }
    }
    __finally
    {
        --g_gameCallDepth;
    }
    return completed;
}
} // namespace detail

bool IsInsideGameCall()
{
    return g_gameCallDepth > 0;
}

bool HasFaulted()
{
    return g_faulted.load();
}

void ResetFaultState()
{
    g_faulted.store(false);
}
} // namespace spl::rage
