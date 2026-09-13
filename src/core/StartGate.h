#pragma once

namespace spl
{
/// Waits for the moment GTA5.exe's code is decrypted and about to start, then hands over to
/// Application::TryStartEarly.
///
/// The ASI is loaded while GTA5.exe's imports are resolved, before the game's code is decrypted,
/// so nothing can be hooked from DllMain. Arm() instead points two of the game's kernel32
/// imports that its C runtime calls on startup, GetCommandLineA and GetStartupInfoW, at detours
/// that ask the application to start. Once it has started, declined or given up, the imports go
/// back to what they were.
///
/// Arm() only patches two pointers, so it is safe to call from DllMain.
void ArmStartGate();

/// Puts the imports back if the gate is still armed. For DLL_PROCESS_DETACH.
void DisarmStartGate();
} // namespace spl
