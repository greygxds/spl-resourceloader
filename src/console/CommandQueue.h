#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace spl::console
{
/// Lines typed on the console thread, waiting for the script thread to run them. Game calls
/// are only valid on the script thread, so the reader never runs a command itself.
class CommandQueue
{
public:
    /// Oldest lines are dropped beyond this, so a stuck script thread cannot grow the queue.
    static constexpr std::size_t kMaxPendingLines = 64;

    void Push(std::string line)
    {
        const std::lock_guard lock{m_mutex};
        if (m_lines.size() == kMaxPendingLines)
        {
            m_lines.pop_front();
        }
        m_lines.push_back(std::move(line));
    }

    /// Everything queued so far, oldest first, leaving the queue empty.
    [[nodiscard]] std::vector<std::string> TakeAll()
    {
        const std::lock_guard lock{m_mutex};
        std::vector<std::string> lines{std::make_move_iterator(m_lines.begin()),
                                       std::make_move_iterator(m_lines.end())};
        m_lines.clear();
        return lines;
    }

private:
    std::mutex m_mutex;
    std::deque<std::string> m_lines;
};
} // namespace spl::console
