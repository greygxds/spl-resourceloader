#include "memory/CodePatch.h"

#include <cstring>
#include <utility>

#include "logging/Logger.h"
#include "platform/Win32.h"

namespace spl::memory
{
namespace
{
constexpr uint8_t kNopOpcode = 0x90;
constexpr size_t kCallLengthBytes = 5; ///< E8 rel32
constexpr ptrdiff_t kRel32Reach = 0x7FFF0000;

/// "jmp qword ptr [rip+0]" followed by the absolute target: 14 bytes, no register touched.
constexpr size_t kStubLengthBytes = 14;

[[nodiscard]] bool IsReadable(const MEMORY_BASIC_INFORMATION& info)
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
           (info.Protect & kReadable) != 0;
}

/// Every region the range touches has to be readable: a patch can straddle a region boundary.
[[nodiscard]] bool ReadCode(uintptr_t address, size_t sizeBytes, std::vector<uint8_t>& out)
{
    const uintptr_t end = address + sizeBytes;
    if (end < address)
    {
        return false;
    }
    for (uintptr_t cursor = address; cursor < end;)
    {
        MEMORY_BASIC_INFORMATION info = {};
        if (::VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) == 0 ||
            !IsReadable(info))
        {
            return false;
        }
        cursor = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    out.assign(reinterpret_cast<const uint8_t*>(address),
               reinterpret_cast<const uint8_t*>(address) + sizeBytes);
    return true;
}

/// Writes over code that is normally read-only, then restores the page protection and lets
/// the CPU know the instruction bytes changed.
[[nodiscard]] bool WriteCode(uintptr_t address, std::span<const uint8_t> bytes)
{
    void* target = reinterpret_cast<void*>(address);
    DWORD previous = 0;
    if (::VirtualProtect(target, bytes.size(), PAGE_EXECUTE_READWRITE, &previous) == 0)
    {
        return false;
    }
    std::memcpy(target, bytes.data(), bytes.size());
    ::VirtualProtect(target, bytes.size(), previous, &previous);
    ::FlushInstructionCache(::GetCurrentProcess(), target, bytes.size());
    return true;
}

/// A 14-byte absolute-jump stub within rel32 reach of anchor, or nullptr when no free page
/// could be reserved near enough. Probing upwards first keeps stubs close together.
[[nodiscard]] void* AllocateStubNear(uintptr_t anchor, void* target)
{
    SYSTEM_INFO systemInfo = {};
    ::GetSystemInfo(&systemInfo);
    const uintptr_t granularity = systemInfo.dwAllocationGranularity;
    const uintptr_t start = anchor - (anchor % granularity);

    void* stub = nullptr;
    for (uintptr_t distance = granularity;
         distance < static_cast<uintptr_t>(kRel32Reach) && stub == nullptr; distance += granularity)
    {
        for (const uintptr_t candidate : {start + distance, start - distance})
        {
            if (candidate < granularity)
            {
                continue;
            }
            stub = ::VirtualAlloc(reinterpret_cast<void*>(candidate), kStubLengthBytes,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (stub != nullptr)
            {
                break;
            }
        }
    }
    if (stub == nullptr)
    {
        return nullptr;
    }

    uint8_t code[kStubLengthBytes] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const uint64_t absolute = reinterpret_cast<uint64_t>(target);
    std::memcpy(code + 6, &absolute, sizeof(absolute));
    std::memcpy(stub, code, sizeof(code));

    DWORD previous = 0;
    ::VirtualProtect(stub, kStubLengthBytes, PAGE_EXECUTE_READ, &previous);
    ::FlushInstructionCache(::GetCurrentProcess(), stub, kStubLengthBytes);
    return stub;
}
} // namespace

CodePatch::CodePatch(CodePatch&& other) noexcept
    : m_name(std::move(other.m_name)), m_address(other.m_address),
      m_original(std::move(other.m_original)), m_stub(other.m_stub), m_applied(other.m_applied)
{
    other.m_stub = nullptr;
    other.m_applied = false;
}

CodePatch& CodePatch::operator=(CodePatch&& other) noexcept
{
    if (this != &other)
    {
        Restore();
        m_name = std::move(other.m_name);
        m_address = other.m_address;
        m_original = std::move(other.m_original);
        m_stub = other.m_stub;
        m_applied = other.m_applied;
        other.m_stub = nullptr;
        other.m_applied = false;
    }
    return *this;
}

CodePatch::~CodePatch()
{
    Restore();
}

Result<CodePatch> CodePatch::Write(std::string name, uintptr_t address,
                                   std::span<const uint8_t> bytes,
                                   std::span<const uint8_t> expectedBytes)
{
    if (address == 0 || bytes.empty())
    {
        return MakeError(ErrorCode::InvalidArgument, "patch '{}' has no address or no bytes", name);
    }
    if (!expectedBytes.empty() && expectedBytes.size() != bytes.size())
    {
        return MakeError(ErrorCode::InvalidArgument, "patch '{}' expects {} bytes but writes {}",
                         name, expectedBytes.size(), bytes.size());
    }

    CodePatch patch;
    if (!ReadCode(address, bytes.size(), patch.m_original))
    {
        return MakeError(ErrorCode::AccessDenied, "patch '{}' cannot read the code at {:#x}", name,
                         address);
    }
    if (!expectedBytes.empty() &&
        std::memcmp(patch.m_original.data(), expectedBytes.data(), expectedBytes.size()) != 0)
    {
        return MakeError(ErrorCode::NotFound,
                         "patch '{}' found unexpected bytes at {:#x}; the signature no longer "
                         "points at the intended instruction",
                         name, address);
    }
    if (!WriteCode(address, bytes))
    {
        return MakeError(ErrorCode::AccessDenied, "patch '{}' cannot write the code at {:#x}", name,
                         address);
    }

    patch.m_name = std::move(name);
    patch.m_address = address;
    patch.m_applied = true;
    SPL_LOG_DEBUG(Hook, "Applied patch '{}' at {:#x} ({} bytes)", patch.m_name, address,
                  patch.m_original.size());
    return patch;
}

Result<CodePatch> CodePatch::Nop(std::string name, uintptr_t address, size_t count)
{
    if (count == 0)
    {
        return MakeError(ErrorCode::InvalidArgument, "patch '{}' would nop out zero bytes", name);
    }
    const std::vector<uint8_t> nops(count, kNopOpcode);
    return Write(std::move(name), address, nops);
}

Result<CodePatch> CodePatch::WriteCall(std::string name, uintptr_t address, void* target)
{
    if (target == nullptr)
    {
        return MakeError(ErrorCode::InvalidArgument, "patch '{}' has no call target", name);
    }

    void* stub = nullptr;
    uintptr_t destination = reinterpret_cast<uintptr_t>(target);
    ptrdiff_t relative =
        static_cast<ptrdiff_t>(destination) - static_cast<ptrdiff_t>(address + kCallLengthBytes);
    if (relative > kRel32Reach || relative < -kRel32Reach)
    {
        stub = AllocateStubNear(address, target);
        if (stub == nullptr)
        {
            return MakeError(ErrorCode::Unavailable,
                             "patch '{}' cannot reach {:#x} from {:#x} and no trampoline could "
                             "be allocated in between",
                             name, destination, address);
        }
        destination = reinterpret_cast<uintptr_t>(stub);
        relative = static_cast<ptrdiff_t>(destination) -
                   static_cast<ptrdiff_t>(address + kCallLengthBytes);
    }

    std::vector<uint8_t> bytes(kCallLengthBytes, 0);
    bytes[0] = 0xE8;
    const int32_t rel32 = static_cast<int32_t>(relative);
    std::memcpy(bytes.data() + 1, &rel32, sizeof(rel32));

    Result<CodePatch> patch = Write(std::move(name), address, bytes);
    if (!patch)
    {
        if (stub != nullptr)
        {
            ::VirtualFree(stub, 0, MEM_RELEASE);
        }
        return patch;
    }
    patch.GetValue().m_stub = stub;
    return patch;
}

void CodePatch::Restore()
{
    if (!m_applied)
    {
        return;
    }
    m_applied = false; // before the write, so a failed restore is not retried forever
    if (!WriteCode(m_address, m_original))
    {
        SPL_LOG_ERROR(Hook, "Could not restore patch '{}' at {:#x}", m_name, m_address);
    }
    else
    {
        SPL_LOG_DEBUG(Hook, "Restored patch '{}' at {:#x}", m_name, m_address);
    }
    if (m_stub != nullptr)
    {
        ::VirtualFree(m_stub, 0, MEM_RELEASE);
        m_stub = nullptr;
    }
}

PatchRegistry::~PatchRegistry()
{
    RestoreAll();
}

void PatchRegistry::Add(CodePatch patch)
{
    m_patches.push_back(std::move(patch));
}

Result<void> PatchRegistry::Apply(std::string name, uintptr_t address,
                                  std::span<const uint8_t> bytes,
                                  std::span<const uint8_t> expectedBytes)
{
    Result<CodePatch> patch = CodePatch::Write(std::move(name), address, bytes, expectedBytes);
    if (!patch)
    {
        return patch.GetError();
    }
    Add(std::move(patch.GetValue()));
    return {};
}

void PatchRegistry::RestoreAll()
{
    // Reverse order, so overlapping patches unwind the way they were applied.
    while (!m_patches.empty())
    {
        m_patches.back().Restore();
        m_patches.pop_back();
    }
}

void PatchRegistry::Dump() const
{
    SPL_LOG_DEBUG(Hook, "{} code patch(es) applied", m_patches.size());
    for (const CodePatch& patch : m_patches)
    {
        SPL_LOG_DEBUG(Hook, "  '{}' at {:#x} ({} bytes, {})", patch.GetName(), patch.GetAddress(),
                      patch.GetSizeBytes(), patch.IsApplied() ? "applied" : "restored");
    }
}
} // namespace spl::memory
