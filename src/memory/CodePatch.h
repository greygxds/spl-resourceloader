#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/Result.h"

namespace spl::memory
{
/// A reversible write into executable memory. Construction applies the patch, Restore() puts
/// the original bytes back, and the destructor restores as well, so a patch never outlives
/// the object that owns it. Move-only: the moved-from patch is no longer applied.
class CodePatch
{
public:
    CodePatch() = default;
    CodePatch(const CodePatch&) = delete;
    CodePatch& operator=(const CodePatch&) = delete;
    CodePatch(CodePatch&& other) noexcept;
    CodePatch& operator=(CodePatch&& other) noexcept;
    ~CodePatch();

    /// Writes bytes over the code at address. When expectedBytes is not empty, the memory
    /// must currently hold exactly those bytes, or the patch is refused: that is the guard
    /// against a signature that drifted onto the wrong instruction.
    [[nodiscard]] static Result<CodePatch> Write(std::string name, uintptr_t address,
                                                 std::span<const uint8_t> bytes,
                                                 std::span<const uint8_t> expectedBytes = {});

    /// Replaces count bytes with 0x90.
    [[nodiscard]] static Result<CodePatch> Nop(std::string name, uintptr_t address, size_t count);

    /// Redirects the "E8 rel32" call at address to target. A rel32 only reaches ±2 GB, and our
    /// DLL can sit further away than that from the game image, so when the target is out of
    /// reach the call goes to a "jmp [rip+0]" stub allocated next to the call site instead.
    [[nodiscard]] static Result<CodePatch> WriteCall(std::string name, uintptr_t address,
                                                     void* target);

    /// Puts the original bytes back. Idempotent, so calling it twice is not an error.
    void Restore();

    [[nodiscard]] bool IsApplied() const
    {
        return m_applied;
    }

    [[nodiscard]] const std::string& GetName() const
    {
        return m_name;
    }

    [[nodiscard]] uintptr_t GetAddress() const
    {
        return m_address;
    }

    [[nodiscard]] size_t GetSizeBytes() const
    {
        return m_original.size();
    }

    /// The original bytes, for diagnostics and for tests that check the restore path.
    [[nodiscard]] std::span<const uint8_t> GetOriginalBytes() const
    {
        return m_original;
    }

private:
    std::string m_name;
    uintptr_t m_address = 0;
    std::vector<uint8_t> m_original;
    void* m_stub = nullptr; ///< the far-call trampoline, when WriteCall needed one
    bool m_applied = false;
};

/// Owns the patches one subsystem applied, so they are restored together and can be listed
/// in a diagnostic dump. Not a singleton: the subsystem that applies patches owns its
/// registry, which keeps the lifetime obvious and the class testable.
class PatchRegistry
{
public:
    PatchRegistry() = default;
    PatchRegistry(const PatchRegistry&) = delete;
    PatchRegistry& operator=(const PatchRegistry&) = delete;
    PatchRegistry(PatchRegistry&&) = default;
    PatchRegistry& operator=(PatchRegistry&&) = default;
    ~PatchRegistry();

    /// Takes ownership of an applied patch. A patch that failed is not added: pass the
    /// Result's value only after checking it.
    void Add(CodePatch patch);

    /// Applies a patch and takes ownership of it in one step. The error is returned as it
    /// is, so the caller decides whether a failed patch is fatal.
    [[nodiscard]] Result<void> Apply(std::string name, uintptr_t address,
                                     std::span<const uint8_t> bytes,
                                     std::span<const uint8_t> expectedBytes = {});

    /// Restores every patch, most recent first, and forgets them.
    void RestoreAll();

    /// Logs one line per patch on the hook channel: name, address and size.
    void Dump() const;

    [[nodiscard]] size_t GetCount() const
    {
        return m_patches.size();
    }

private:
    std::vector<CodePatch> m_patches;
};
} // namespace spl::memory
