#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spl::console
{
/// Splits a typed line into words. Whitespace separates words; double quotes keep spaces inside
/// one ("load \"my map\""); a quote that is never closed runs to the end of the line.
[[nodiscard]] std::vector<std::string> SplitCommandLine(std::string_view line);

/// What a command prints, one entry per line.
using CommandOutput = std::vector<std::string>;

/// One console command. The handler gets the words after the command name.
struct Command
{
    std::string name;    ///< lower case, what the user types
    std::string usage;   ///< "maps [radius]", shown by help and on a wrong argument count
    std::string summary; ///< one line for help
    std::size_t minArguments = 0;
    std::size_t maxArguments = std::numeric_limits<std::size_t>::max();
    std::function<CommandOutput(std::span<const std::string> arguments)> run;
};

/// The commands the console knows, and the dispatcher that runs a typed line. Pure: it runs a
/// handler on the calling thread and returns its lines, so the caller decides where commands run
/// (the script thread) and where output goes (the log).
class CommandRegistry
{
public:
    /// Adds "help", which lists every command or describes one.
    CommandRegistry();

    /// False, and nothing changes, when a command of that name exists or the name is empty.
    bool Add(Command command);

    /// Runs one typed line. An empty line prints nothing; an unknown command or a wrong
    /// argument count prints why instead of running anything.
    [[nodiscard]] CommandOutput Execute(std::string_view line) const;

    /// The command of that name, in any case, or nullptr.
    [[nodiscard]] const Command* Find(std::string_view name) const;

    /// Every command, in the order it was added.
    [[nodiscard]] std::span<const Command> All() const
    {
        return m_commands;
    }

private:
    [[nodiscard]] CommandOutput Help(std::span<const std::string> arguments) const;

    std::vector<Command> m_commands;
};
} // namespace spl::console
