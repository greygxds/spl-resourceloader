#pragma once

#include <atomic>
#include <memory>

#include "console/CommandQueue.h"

namespace spl::console
{
/// Reads lines typed into the loader's console window on a thread of its own and queues them.
///
/// The thread is never joined: Stop() runs from DllMain, where waiting on a thread deadlocks on
/// the loader lock. It cancels the pending read instead and lets the thread end by itself.
class ConsoleInput
{
public:
    ConsoleInput() = default;
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;
    ConsoleInput(ConsoleInput&&) = delete;
    ConsoleInput& operator=(ConsoleInput&&) = delete;
    ~ConsoleInput();

    /// Starts reading when this process has a console window. False when it has none, or when
    /// the input could not be opened. Calling it again while running does nothing.
    bool Start();

    /// Cancels the read and lets the thread exit. Safe to call more than once.
    void Stop();

    [[nodiscard]] CommandQueue& Queue()
    {
        return m_state->queue;
    }

    /// Shared with the reader thread, so a thread that outlives this object still has valid
    /// state. Public only so the thread function can name it.
    struct State
    {
        CommandQueue queue;
        std::atomic<bool> stopping = false;
        void* input = nullptr; ///< CONIN$, open for the rest of the process
    };

private:
    std::shared_ptr<State> m_state = std::make_shared<State>();
    bool m_running = false;
};
} // namespace spl::console
