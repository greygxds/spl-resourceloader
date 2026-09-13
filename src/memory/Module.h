#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spl::memory
{
/// One PE section of a loaded module, as it sits in memory.
struct Section
{
    std::string name; ///< ".text"; the PE field is 8 bytes and not always terminated
    uintptr_t begin = 0;
    size_t sizeBytes = 0;
    bool executable = false;
};

/// Which sections a pattern scan covers. Most signatures match code; a few match tables that
/// live in the read-only data sections instead.
enum class SectionKind
{
    Code, ///< executable sections
    Data, ///< non-executable sections: .rdata, .data
    Any
};

/// A loaded module, inspected through its in-memory PE headers. Read-only and copyable: it
/// holds no handle, only the addresses and sizes it parsed.
class Module
{
public:
    /// The process image, GTA5.exe in the game and spl_tests.exe under test.
    [[nodiscard]] static Module Main();

    /// std::nullopt when no module of that name is loaded.
    [[nodiscard]] static std::optional<Module> Find(std::wstring_view name);

    /// A PE image laid out in memory by someone other than the loader, such as spl_sigcheck
    /// mapping GTA5.exe from disk. base must stay valid for as long as the Module is used.
    [[nodiscard]] static std::optional<Module> FromImage(void* base, std::filesystem::path path);

    [[nodiscard]] uintptr_t GetBase() const
    {
        return m_base;
    }

    /// SizeOfImage from the optional header, so it covers every section.
    [[nodiscard]] size_t GetSizeBytes() const
    {
        return m_sizeBytes;
    }

    [[nodiscard]] std::span<const Section> GetSections() const
    {
        return m_sections;
    }

    /// nullptr when the module has no section of that name. Non-owning.
    [[nodiscard]] const Section* FindSection(std::string_view name) const;

    /// Every section marked executable. Empty for a packed image whose sections lost the
    /// flag, which is why GetScanRegions() exists.
    [[nodiscard]] std::vector<Section> GetExecutableSections() const;

    /// What a pattern scan of kind should cover. Falls back to the whole image when no section
    /// qualifies (GTA5.exe has packer-renamed sections on some builds).
    [[nodiscard]] std::vector<Section> GetScanRegions(SectionKind kind = SectionKind::Code) const;

    [[nodiscard]] bool Contains(uintptr_t address) const
    {
        return address >= m_base && address < m_base + m_sizeBytes;
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const
    {
        return m_path;
    }

    /// The module's file name, for log lines such as "GTA5.exe+0x1234567".
    [[nodiscard]] std::string GetFileName() const;

    /// The bytes of one region of this module. The span is only valid while the module
    /// stays loaded.
    [[nodiscard]] std::span<const std::byte> GetBytes(const Section& section) const;

private:
    Module() = default;

    /// Parses the PE headers at base. Returns std::nullopt for anything that does not look
    /// like a 64-bit PE image.
    [[nodiscard]] static std::optional<Module> FromBase(void* base);

    uintptr_t m_base = 0;
    size_t m_sizeBytes = 0;
    std::vector<Section> m_sections;
    std::filesystem::path m_path;
};
} // namespace spl::memory
