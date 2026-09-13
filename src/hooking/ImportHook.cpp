#include "hooking/ImportHook.h"

#include <string>
#include <utility>

#include "platform/Win32.h"
#include "util/Strings.h"

namespace spl::hooking
{
namespace
{
/// Writes one pointer into an IAT slot, which the loader usually leaves read-only.
[[nodiscard]] bool WriteSlot(uintptr_t slot, void* value)
{
    DWORD oldProtect = 0;
    if (::VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE,
                         &oldProtect) == 0)
    {
        return false;
    }
    *reinterpret_cast<void**>(slot) = value;
    DWORD ignored = 0;
    ::VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), oldProtect, &ignored);
    return true;
}
} // namespace

ImportHook::ImportHook(ImportHook&& other) noexcept
    : m_slots(std::move(other.m_slots)), m_original(other.m_original), m_detour(other.m_detour)
{
    other.m_slots.clear();
}

ImportHook& ImportHook::operator=(ImportHook&& other) noexcept
{
    if (this != &other)
    {
        Restore();
        m_slots = std::move(other.m_slots);
        m_original = other.m_original;
        m_detour = other.m_detour;
        other.m_slots.clear();
    }
    return *this;
}

ImportHook::~ImportHook()
{
    Restore();
}

Result<ImportHook> ImportHook::Install(uintptr_t moduleBase, std::string_view dll,
                                       std::string_view function, void* detour)
{
    const std::string dllName{dll};
    const std::string functionName{function};
    HMODULE const exporter = ::GetModuleHandleA(dllName.c_str());
    const void* const address =
        exporter != nullptr ? ::GetProcAddress(exporter, functionName.c_str()) : nullptr;
    if (address == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "{}!{} does not exist", dll, function);
    }

    // Slots are matched by the address they hold, and by the name table where the executable
    // kept one.
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
    {
        return MakeError(ErrorCode::NotFound, "the module imports nothing");
    }

    ImportHook hook;
    hook.m_detour = detour; // set first, so a failure half way still restores what was written
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
             moduleBase + directory.VirtualAddress);
         descriptor->Name != 0; ++descriptor)
    {
        const auto* const name = reinterpret_cast<const char*>(moduleBase + descriptor->Name);
        if (!util::EqualsIgnoreCase(name, dll) || descriptor->FirstThunk == 0)
        {
            continue;
        }
        const auto* names = descriptor->OriginalFirstThunk != 0
                                ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                                      moduleBase + descriptor->OriginalFirstThunk)
                                : nullptr;
        for (auto* thunk =
                 reinterpret_cast<IMAGE_THUNK_DATA64*>(moduleBase + descriptor->FirstThunk);
             thunk->u1.Function != 0; ++thunk, names = names != nullptr ? names + 1 : nullptr)
        {
            // By name as well: another plugin may already have pointed the slot elsewhere.
            const bool namedHere = names != nullptr &&
                                   !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                                   functionName == reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                                       moduleBase + names->u1.AddressOfData)
                                                       ->Name;
            if (reinterpret_cast<const void*>(thunk->u1.Function) != address && !namedHere)
            {
                continue;
            }
            const auto slot = reinterpret_cast<uintptr_t>(&thunk->u1.Function);
            if (hook.m_original == nullptr)
            {
                hook.m_original = reinterpret_cast<void*>(thunk->u1.Function);
            }
            if (!WriteSlot(slot, detour))
            {
                return MakeError(ErrorCode::AccessDenied,
                                 "the import slot of {}!{} is not writable", dll, function);
            }
            hook.m_slots.push_back(slot);
        }
    }

    if (hook.m_slots.empty())
    {
        return MakeError(ErrorCode::NotFound, "the module does not import {}!{}", dll, function);
    }
    return hook;
}

void ImportHook::Restore()
{
    for (const uintptr_t slot : m_slots)
    {
        if (*reinterpret_cast<void**>(slot) == m_detour)
        {
            (void)WriteSlot(slot, m_original);
        }
    }
    m_slots.clear();
}
} // namespace spl::hooking
