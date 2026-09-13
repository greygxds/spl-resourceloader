#pragma once

#include "console/CommandRegistry.h"

namespace spl
{
class Application;

/// Adds the loader's own commands: status, resources, find, maps. They read game memory, so the
/// registry has to be executed on the script thread (Application::Tick does).
void RegisterConsoleCommands(console::CommandRegistry& registry, Application& application);
} // namespace spl
