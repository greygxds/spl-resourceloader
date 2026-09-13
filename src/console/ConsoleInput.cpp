#include "console/ConsoleInput.h"

#include <array>
#include <memory>
#include <string>
#include <thread>

#include "platform/Win32.h"
#include "util/Strings.h"

namespace spl::console
{
namespace
{
/// Longer than anyone types; a longer line arrives in pieces and is joined.
constexpr std::size_t kReadBufferChars = 512;

/// Line input with echo, and without quick-edit: selecting text in a quick-edit console pauses
/// every write to it, which would stall the logger and with it the game.
constexpr DWORD kInputMode = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                             ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS;

[[nodiscard]] std::string ToUtf8(const wchar_t* text, int length)
{
    if (length <= 0)
    {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

void ReadLines(const std::shared_ptr<ConsoleInput::State>& state)
{
    std::array<wchar_t, kReadBufferChars> buffer{};
    std::string pending;
    while (!state->stopping)
    {
        DWORD read = 0;
        if (::ReadConsoleW(state->input, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                           nullptr) == 0)
        {
            break; // the console went away
        }
        if (state->stopping)
        {
            break;
        }

        pending += ToUtf8(buffer.data(), static_cast<int>(read));
        for (std::size_t end = pending.find('\n'); end != std::string::npos;
             end = pending.find('\n'))
        {
            std::string line = util::Trim(std::string_view{pending}.substr(0, end));
            pending.erase(0, end + 1);
            if (!line.empty())
            {
                state->queue.Push(std::move(line));
            }
        }
    }
    // The handle stays open: Stop() may still write to it, and the process is ending anyway.
}
} // namespace

ConsoleInput::~ConsoleInput()
{
    Stop();
}

bool ConsoleInput::Start()
{
    if (m_running)
    {
        return true;
    }
    if (::GetConsoleWindow() == nullptr)
    {
        return false;
    }

    const HANDLE input =
        ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    ::SetConsoleMode(input, kInputMode);

    m_state->input = input;
    m_state->stopping = false;
    std::thread{[state = m_state] { ReadLines(state); }}.detach();
    m_running = true;
    return true;
}

void ConsoleInput::Stop()
{
    if (!m_running)
    {
        return;
    }
    m_running = false;
    m_state->stopping = true;

    // A blocked ReadConsoleW only returns on a line, so hand it an empty one.
    INPUT_RECORD enter{};
    enter.EventType = KEY_EVENT;
    enter.Event.KeyEvent.bKeyDown = TRUE;
    enter.Event.KeyEvent.wRepeatCount = 1;
    enter.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    enter.Event.KeyEvent.uChar.UnicodeChar = L'\r';
    std::array<INPUT_RECORD, 2> records{enter, enter};
    records[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD written = 0;
    ::WriteConsoleInputW(m_state->input, records.data(), static_cast<DWORD>(records.size()),
                         &written);
}
} // namespace spl::console
