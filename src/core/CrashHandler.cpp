#include "core/CrashHandler.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "core/CrashReport.h"
#include "logging/Logger.h"
#include "platform/Win32.h"
#include "rage/SafeCall.h"
#include "util/Strings.h"

// After Windows.h, which it depends on.
#include <DbgHelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace spl
{
namespace
{
struct HandlerState
{
    CrashHandler::Settings settings;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    std::atomic<bool> installed = false;
    std::atomic_flag reporting = ATOMIC_FLAG_INIT; ///< one report per process, whatever happens
};

HandlerState& GetState()
{
    static HandlerState state;
    return state;
}

HMODULE ModuleOf(const void* address)
{
    HMODULE module = nullptr;
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (::GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module) == FALSE)
    {
        return nullptr;
    }
    return module;
}

HMODULE OwnModule()
{
    static const HMODULE module = ModuleOf(reinterpret_cast<const void*>(&GetState));
    return module;
}

/// "resourceLoader.asi+0x1234", or the bare address when no module owns it.
std::string DescribeAddress(const void* address)
{
    const HMODULE module = ModuleOf(address);
    if (module == nullptr)
    {
        return fmt::format("{:#x}", reinterpret_cast<uintptr_t>(address));
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
    const std::filesystem::path file{std::wstring_view{path, length}};
    return fmt::format("{}+{:#x}", util::ToUtf8(file.filename()),
                       reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
}

std::string UtcTimestamp()
{
    SYSTEMTIME time{};
    ::GetSystemTime(&time);
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay,
                       time.wHour, time.wMinute, time.wSecond);
}

bool WriteMinidump(const std::filesystem::path& file, EXCEPTION_POINTERS* exception)
{
    const HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{.ThreadId = ::GetCurrentThreadId(),
                                               .ExceptionPointers = exception,
                                               .ClientPointers = FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithDataSegs |
                                                 MiniDumpWithHandleData);
    const BOOL written = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), handle,
                                             type, &information, nullptr, nullptr);
    ::CloseHandle(handle);
    return written != FALSE;
}

void Report(HandlerState& state, EXCEPTION_POINTERS* exception, bool inGameCall)
{
    CrashReportInfo info = state.settings.describe ? state.settings.describe() : CrashReportInfo{};
    info.exceptionCode = exception->ExceptionRecord->ExceptionCode;
    info.faultAddress = DescribeAddress(exception->ExceptionRecord->ExceptionAddress);
    info.inGameCall = inGameCall;
    info.timestamp = UtcTimestamp();
    if (std::optional<std::vector<std::string>> breadcrumbs = logging::TryGetBreadcrumbs())
    {
        info.breadcrumbs = std::move(*breadcrumbs);
    }

    if (state.settings.writeMinidump && WriteMinidump(state.settings.dumpFile, exception))
    {
        info.minidump = util::ToUtf8(state.settings.dumpFile);
    }

    {
        std::ofstream stream{state.settings.reportFile, std::ios::binary | std::ios::trunc};
        stream << FormatCrashReport(info);
    }

    if (state.settings.onCrash)
    {
        state.settings.onCrash(info);
    }

    SPL_LOG_CRITICAL(Core, "The game crashed at {} during {}; details in '{}'", info.faultAddress,
                     info.stage.empty() ? std::string{"no loader stage"} : info.stage,
                     util::ToUtf8(state.settings.reportFile));
    logging::Flush();
}

/// Kept out of the filter itself, so the filter's frame owns nothing with a destructor and a
/// throw from a formatter is caught here.
bool TryReport(HandlerState& state, EXCEPTION_POINTERS* exception, bool inGameCall)
{
    try
    {
        Report(state, exception, inGameCall);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* exception)
{
    HandlerState& state = GetState();
    if (state.installed && exception != nullptr && exception->ExceptionRecord != nullptr)
    {
        const bool inGameCall = rage::IsInsideGameCall();
        const bool ours =
            inGameCall || ModuleOf(exception->ExceptionRecord->ExceptionAddress) == OwnModule();
        if (ours && !state.reporting.test_and_set())
        {
            static_cast<void>(TryReport(state, exception, inGameCall));
        }
    }
    return state.previous != nullptr ? state.previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void CrashHandler::Install(Settings settings)
{
    HandlerState& state = GetState();
    state.settings = std::move(settings);
    if (state.installed.exchange(true))
    {
        return;
    }
    state.previous = ::SetUnhandledExceptionFilter(&Filter);
}

void CrashHandler::EnsureFirst()
{
    HandlerState& state = GetState();
    if (!state.installed)
    {
        return;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER current = ::SetUnhandledExceptionFilter(&Filter);
    if (current != &Filter)
    {
        state.previous = current; // the game replaced ours; chain to its filter from now on
    }
}

void CrashHandler::Uninstall()
{
    HandlerState& state = GetState();
    if (!state.installed.exchange(false))
    {
        return;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER current = ::SetUnhandledExceptionFilter(state.previous);
    if (current != &Filter)
    {
        ::SetUnhandledExceptionFilter(current); // someone chained after us; leave theirs in place
    }
}
} // namespace spl
