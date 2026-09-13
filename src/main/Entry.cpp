#include <main.h> // ScriptHookV SDK

#include "core/Application.h"
#include "core/StartGate.h"
#include "platform/Win32.h"

namespace
{
void ScriptMain()
{
    spl::Application& app = spl::Application::Instance();
    if (!app.Initialize())
    {
        return; // stays loaded, does nothing
    }
    while (true)
    {
        app.Tick();
        WAIT(0);
    }
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        spl::Application::SetModuleHandle(module);
        spl::ArmStartGate(); // starts the loader with the game
        scriptRegister(module, ScriptMain);
        break;
    case DLL_PROCESS_DETACH:
        spl::DisarmStartGate();
        spl::Application::Instance().Shutdown();
        scriptUnregister(module);
        break;
    default:
        break;
    }
    return TRUE;
}
