#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/Win32.h"

namespace spl::rage
{
/// The committed, readable bytes from address to the end of its memory region, or 0.
///
/// SafeCall stops every game call for the session once it catches a fault, so memory whose
/// layout is only a guess (a diagnostic walk, a verification probe) is checked with this first:
/// a wrong guess then becomes a refused check instead of a dead bridge.
[[nodiscard]] inline std::size_t ReadableBytesAt(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info = {};
    if (address == 0 ||
        ::VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT)
    {
        return 0;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kReadable) == 0 || (info.Protect & PAGE_GUARD) != 0)
    {
        return 0;
    }
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return regionEnd - address;
}

/// True when sizeBytes bytes at address can be read without faulting. A span that crosses into
/// a second region counts as unreadable, which only ever refuses too much.
[[nodiscard]] inline bool IsReadableMemory(uintptr_t address, std::size_t sizeBytes)
{
    return ReadableBytesAt(address) >= sizeBytes;
}
} // namespace spl::rage
