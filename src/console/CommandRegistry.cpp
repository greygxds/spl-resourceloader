#include "console/CommandRegistry.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "util/Strings.h"

namespace spl::console
{
namespace
{
[[nodiscard]] bool IsSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}
} // namespace

std::vector<std::string> SplitCommandLine(std::string_view line)
{
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    bool quoted = false;

    for (const char character : line)
    {
        if (character == '"')
        {
            quoted = !quoted;
            inWord = true; // "" is an empty word, not nothing
            continue;
        }
        if (IsSpace(character) && !quoted)
        {
            if (inWord)
            {
                words.push_back(std::move(word));
                word.clear();
                inWord = false;
            }
            continue;
        }
        word.push_back(character);
        inWord = true;
    }
    if (inWord)
    {
        words.push_back(std::move(word));
    }
    return words;
}

CommandRegistry::CommandRegistry()
{
    Add(Command{.name = "help",
                .usage = "help [command]",
                .summary = "lists the commands, or describes one",
                .maxArguments = 1,
                .run = [this](std::span<const std::string> arguments) { return Help(arguments); }});
}

bool CommandRegistry::Add(Command command)
{
    command.name = util::ToLower(util::Trim(command.name));
    if (command.name.empty() || !command.run || Find(command.name) != nullptr)
    {
        return false;
    }
    m_commands.push_back(std::move(command));
    return true;
}

const Command* CommandRegistry::Find(std::string_view name) const
{
    const std::string lowered = util::ToLower(name);
    const auto match = std::ranges::find(m_commands, lowered, &Command::name);
    return match != m_commands.end() ? &*match : nullptr;
}

CommandOutput CommandRegistry::Execute(std::string_view line) const
{
    const std::vector<std::string> words = SplitCommandLine(line);
    if (words.empty())
    {
        return {};
    }

    const Command* const command = Find(words.front());
    if (command == nullptr)
    {
        return {fmt::format("Unknown command '{}'; type 'help' for the list", words.front())};
    }

    const std::span<const std::string> arguments = std::span{words}.subspan(1);
    if (arguments.size() < command->minArguments || arguments.size() > command->maxArguments)
    {
        return {fmt::format("Usage: {}", command->usage)};
    }
    return command->run(arguments);
}

CommandOutput CommandRegistry::Help(std::span<const std::string> arguments) const
{
    if (!arguments.empty())
    {
        const Command* const command = Find(arguments.front());
        if (command == nullptr)
        {
            return {fmt::format("Unknown command '{}'", arguments.front())};
        }
        return {fmt::format("{} — {}", command->usage, command->summary)};
    }

    std::size_t width = 0;
    for (const Command& command : m_commands)
    {
        width = std::max(width, command.usage.size());
    }
    CommandOutput output{"Commands:"};
    for (const Command& command : m_commands)
    {
        output.push_back(fmt::format("  {:<{}}  {}", command.usage, width, command.summary));
    }
    return output;
}
} // namespace spl::console
