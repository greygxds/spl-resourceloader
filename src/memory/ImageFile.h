#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

#include "core/Result.h"
#include "memory/Module.h"

namespace spl::memory
{
/// A 64-bit PE file from disk, laid out the way the loader would map it: headers first, every
/// section at its virtual address. No relocations are applied and no import is resolved, so
/// rip-relative operands and RVAs read as they do in the running game, and nothing runs.
///
/// Used by spl_sigcheck to resolve the signature table against a GTA5.exe without launching it.
class ImageFile
{
public:
    [[nodiscard]] static Result<ImageFile> Load(const std::filesystem::path& file);

    /// A copy of a module as it sits in a running process, for an executable that is packed on
    /// disk. Pages the process will not let us read stay zero. The copy's path is the module's
    /// file on disk, so its version resource can still be read.
    [[nodiscard]] static Result<ImageFile> Capture(std::wstring_view processName);

    /// Valid while this ImageFile is alive.
    [[nodiscard]] const Module& GetModule() const
    {
        return m_module;
    }

private:
    struct VirtualFreeDeleter
    {
        void operator()(void* memory) const;
    };

    ImageFile(std::unique_ptr<void, VirtualFreeDeleter> memory, Module module);

    std::unique_ptr<void, VirtualFreeDeleter> m_memory;
    Module m_module;
};
} // namespace spl::memory
