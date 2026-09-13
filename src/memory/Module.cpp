#include "memory/Module.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "platform/Win32.h"
#include "util/Strings.h"

namespace spl::memory
{
namespace
{
/// The PE section name field is 8 bytes and only terminated when it is shorter than that.
std::string ReadSectionName(const IMAGE_SECTION_HEADER& header)
{
    const char* name = reinterpret_cast<const char*>(header.Name);
    const size_t length = ::strnlen(name, sizeof(header.Name));
    return std::string(name, length);
}

std::filesystem::path QueryModulePath(HMODULE module)
{
    // MAX_PATH is enough for a game install; a longer path simply yields a truncated
    // diagnostic string, never a wrong address.
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length =
        ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
    {
        return {};
    }
    return std::filesystem::path(std::wstring_view(buffer, length));
}
} // namespace

std::optional<Module> Module::FromBase(void* base)
{
    if (base == nullptr)
    {
        return std::nullopt;
    }

    const auto* dosHeader = static_cast<const IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return std::nullopt;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        static_cast<const std::byte*>(base) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return std::nullopt;
    }

    Module module;
    module.m_base = reinterpret_cast<uintptr_t>(base);
    module.m_sizeBytes = ntHeaders->OptionalHeader.SizeOfImage;
    module.m_path = QueryModulePath(static_cast<HMODULE>(base));

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
    module.m_sections.reserve(ntHeaders->FileHeader.NumberOfSections);
    for (WORD index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index)
    {
        const IMAGE_SECTION_HEADER& header = sections[index];
        // VirtualSize can be 0 in hand-written PEs; SizeOfRawData is then the real extent.
        const size_t sizeBytes =
            header.Misc.VirtualSize != 0 ? header.Misc.VirtualSize : header.SizeOfRawData;
        module.m_sections.push_back(Section{
            .name = ReadSectionName(header),
            .begin = module.m_base + header.VirtualAddress,
            .sizeBytes = sizeBytes,
            .executable = (header.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
        });
    }
    return module;
}

Module Module::Main()
{
    std::optional<Module> module = FromBase(::GetModuleHandleW(nullptr));
    // The process image is always a valid PE, so this cannot fail for the main module.
    return module.value();
}

std::optional<Module> Module::FromImage(void* base, std::filesystem::path path)
{
    std::optional<Module> module = FromBase(base);
    if (module)
    {
        module->m_path = std::move(path);
    }
    return module;
}

std::optional<Module> Module::Find(std::wstring_view name)
{
    const std::wstring terminated(name);
    HMODULE handle = ::GetModuleHandleW(terminated.c_str());
    if (handle == nullptr)
    {
        return std::nullopt;
    }
    return FromBase(handle);
}

const Section* Module::FindSection(std::string_view name) const
{
    const auto match = std::ranges::find_if(m_sections, [name](const Section& section)
                                            { return section.name == name; });
    return match != m_sections.end() ? &*match : nullptr;
}

std::vector<Section> Module::GetExecutableSections() const
{
    std::vector<Section> result;
    for (const Section& section : m_sections)
    {
        if (section.executable && section.sizeBytes != 0)
        {
            result.push_back(section);
        }
    }
    return result;
}

std::vector<Section> Module::GetScanRegions(SectionKind kind) const
{
    std::vector<Section> result;
    for (const Section& section : m_sections)
    {
        const bool wanted =
            kind == SectionKind::Any || section.executable == (kind == SectionKind::Code);
        if (wanted && section.sizeBytes != 0)
        {
            result.push_back(section);
        }
    }
    if (!result.empty())
    {
        return result;
    }
    return {
        Section{.name = "<image>", .begin = m_base, .sizeBytes = m_sizeBytes, .executable = true}};
}

std::string Module::GetFileName() const
{
    return m_path.empty() ? std::string("<unknown>") : util::ToUtf8(m_path.filename());
}

std::span<const std::byte> Module::GetBytes(const Section& section) const
{
    return {reinterpret_cast<const std::byte*>(section.begin), section.sizeBytes};
}
} // namespace spl::memory
