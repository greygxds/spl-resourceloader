#include <span>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "console/CommandQueue.h"
#include "console/CommandRegistry.h"

using spl::console::Command;
using spl::console::CommandOutput;
using spl::console::CommandQueue;
using spl::console::CommandRegistry;
using spl::console::SplitCommandLine;

namespace
{
using Words = std::vector<std::string>;

/// A command that prints its arguments back, joined by '|'.
Command Echo(std::size_t minArguments = 0, std::size_t maxArguments = 2)
{
    return Command{.name = "Echo",
                   .usage = "echo [a] [b]",
                   .summary = "prints its arguments",
                   .minArguments = minArguments,
                   .maxArguments = maxArguments,
                   .run = [](std::span<const std::string> arguments)
                   {
                       std::string joined;
                       for (const std::string& argument : arguments)
                       {
                           joined += (joined.empty() ? "" : "|") + argument;
                       }
                       return CommandOutput{joined};
                   }};
}
} // namespace

TEST_CASE("Console: a line splits on whitespace", "[console]")
{
    CHECK(SplitCommandLine("find  prop.ydr ") == Words{"find", "prop.ydr"});
    CHECK(SplitCommandLine("\tmaps\t150\r\n") == Words{"maps", "150"});
    CHECK(SplitCommandLine("").empty());
    CHECK(SplitCommandLine("   ").empty());
}

TEST_CASE("Console: quotes keep spaces inside one word", "[console]")
{
    CHECK(SplitCommandLine("load \"my map\" now") == Words{"load", "my map", "now"});
    CHECK(SplitCommandLine("load \"\"") == Words{"load", ""});
    CHECK(SplitCommandLine("load \"unterminated name") == Words{"load", "unterminated name"});
}

TEST_CASE("Console: a command runs with its arguments, in any case", "[console]")
{
    CommandRegistry registry;
    REQUIRE(registry.Add(Echo()));

    CHECK(registry.Execute("echo one two") == CommandOutput{"one|two"});
    CHECK(registry.Execute("ECHO \"one two\"") == CommandOutput{"one two"});
    CHECK(registry.Execute("   ").empty());
}

TEST_CASE("Console: a wrong argument count prints the usage instead", "[console]")
{
    CommandRegistry registry;
    REQUIRE(registry.Add(Echo(1, 1)));

    CHECK(registry.Execute("echo") == CommandOutput{"Usage: echo [a] [b]"});
    CHECK(registry.Execute("echo a b") == CommandOutput{"Usage: echo [a] [b]"});
    CHECK(registry.Execute("echo a") == CommandOutput{"a"});
}

TEST_CASE("Console: an unknown command says so", "[console]")
{
    const CommandRegistry registry;
    const CommandOutput output = registry.Execute("teleport home");
    REQUIRE(output.size() == 1);
    CHECK(output.front().find("Unknown command 'teleport'") != std::string::npos);
}

TEST_CASE("Console: names are unique, and a command needs a name and a handler", "[console]")
{
    CommandRegistry registry;
    CHECK(registry.Add(Echo()));
    CHECK_FALSE(registry.Add(Echo()));
    CHECK_FALSE(registry.Add(Command{.name = "HELP", .run = Echo().run}));
    CHECK_FALSE(registry.Add(Command{.name = " ", .run = Echo().run}));
    CHECK_FALSE(registry.Add(Command{.name = "nothing"}));
    CHECK(registry.All().size() == 2); // help and echo
}

TEST_CASE("Console: help lists every command, or describes one", "[console]")
{
    CommandRegistry registry;
    REQUIRE(registry.Add(Echo()));

    const CommandOutput all = registry.Execute("help");
    REQUIRE(all.size() == 3);
    CHECK(all[0] == "Commands:");
    CHECK(all[1].find("help [command]") != std::string::npos);
    CHECK(all[2].find("echo [a] [b]") != std::string::npos);
    CHECK(all[2].find("prints its arguments") != std::string::npos);

    const CommandOutput one = registry.Execute("help echo");
    REQUIRE(one.size() == 1);
    CHECK(one.front().find("prints its arguments") != std::string::npos);
    CHECK(registry.Execute("help nope").front().find("Unknown command") != std::string::npos);
}

TEST_CASE("Console: the queue hands lines over in order and drops the oldest when full",
          "[console]")
{
    CommandQueue queue;
    queue.Push("one");
    queue.Push("two");
    CHECK(queue.TakeAll() == Words{"one", "two"});
    CHECK(queue.TakeAll().empty());

    for (std::size_t line = 0; line < CommandQueue::kMaxPendingLines + 2; ++line)
    {
        queue.Push(std::to_string(line));
    }
    const Words lines = queue.TakeAll();
    REQUIRE(lines.size() == CommandQueue::kMaxPendingLines);
    CHECK(lines.front() == "2");
}
