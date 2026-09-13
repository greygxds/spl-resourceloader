#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace spl::hooking
{
/// Points a module's import address table entries for one function at a detour, and puts them
/// back. Unlike a MinHook hook it never touches the function's code, so it works while the
/// module's own code is still encrypted, which is how GTA5.exe looks when our DllMain runs.
class ImportHook
{
public:
    ImportHook() = default;
    ImportHook(const ImportHook&) = delete;
    ImportHook& operator=(const ImportHook&) = delete;
    ImportHook(ImportHook&& other) noexcept;
    ImportHook& operator=(ImportHook&& other) noexcept;
    ~ImportHook();

    /// Redirects every IAT slot of moduleBase that holds the address of dll!function. A module
    /// can import the same DLL through more than one descriptor, and all of them are patched.
    /// Fails when no slot holds it.
    [[nodiscard]] static Result<ImportHook> Install(uintptr_t moduleBase, std::string_view dll,
                                                    std::string_view function, void* detour);

    /// What the slots held before: the function itself, or whoever hooked it before us.
    [[nodiscard]] void* GetOriginal() const
    {
        return m_original;
    }

    /// Puts the original back in every slot that still points at our detour. A slot somebody
    /// else re-hooked in the meantime is left alone, since their hook calls ours.
    void Restore();

    [[nodiscard]] bool IsInstalled() const
    {
        return !m_slots.empty();
    }

private:
    std::vector<uintptr_t> m_slots;
    void* m_original = nullptr;
    void* m_detour = nullptr;
};
} // namespace spl::hooking
