#include "memory/ImageFile.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "core/Result.h"
#include "memory/Module.h"
#include "platform/Win32.h"
#include "util/Strings.h"

// After Windows.h, which it depends on.
#include <TlHelp32.h>

namespace spl::memory
{
void ImageFile::VirtualFreeDeleter::operator()(void* memory) const
{
    if (memory != nullptr)
    {
        ::VirtualFree(memory, 0, MEM_RELEASE);
    }
}

ImageFile::ImageFile(std::unique_ptr<void, VirtualFreeDeleter> memory, Module module)
    : m_memory(std::move(memory)), m_module(std::move(module))
{
}

Result<ImageFile> ImageFile::Load(const std::filesystem::path& file)
{
    const std::string name = util::ToUtf8(file);
    std::ifstream stream{file, std::ios::binary};
    if (!stream)
    {
        return MakeError(ErrorCode::Io, "cannot open '{}'", name);
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>{stream},
                                  std::istreambuf_iterator<char>{}};

    const auto fits = [&bytes](std::size_t offset, std::size_t size)
    { return offset <= bytes.size() && size <= bytes.size() - offset; };

    if (!fits(0, sizeof(IMAGE_DOS_HEADER)))
    {
        return MakeError(ErrorCode::Parse, "'{}' is too small to be an executable", name);
    }
    IMAGE_DOS_HEADER dosHeader{};
    std::memcpy(&dosHeader, bytes.data(), sizeof(dosHeader));
    const auto ntOffset = static_cast<std::size_t>(dosHeader.e_lfanew);
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE || !fits(ntOffset, sizeof(IMAGE_NT_HEADERS64)))
    {
        return MakeError(ErrorCode::Parse, "'{}' is not a PE file", name);
    }
    IMAGE_NT_HEADERS64 ntHeaders{};
    std::memcpy(&ntHeaders, bytes.data() + ntOffset, sizeof(ntHeaders));
    if (ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return MakeError(ErrorCode::Parse, "'{}' is not a 64-bit PE file", name);
    }

    const std::size_t imageSize = ntHeaders.OptionalHeader.SizeOfImage;
    const std::size_t headersSize = ntHeaders.OptionalHeader.SizeOfHeaders;
    if (!fits(0, headersSize) || headersSize > imageSize)
    {
        return MakeError(ErrorCode::Parse, "'{}' has inconsistent header sizes", name);
    }

    std::unique_ptr<void, VirtualFreeDeleter> memory{
        ::VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    if (!memory)
    {
        return MakeError(ErrorCode::Unavailable, "cannot reserve {} bytes to lay out '{}'",
                         imageSize, name);
    }
    auto* image = static_cast<char*>(memory.get());
    std::memcpy(image, bytes.data(), headersSize);

    const std::size_t sectionTable = ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                     ntHeaders.FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionCount = ntHeaders.FileHeader.NumberOfSections;
    if (!fits(sectionTable, sectionCount * sizeof(IMAGE_SECTION_HEADER)))
    {
        return MakeError(ErrorCode::Parse, "'{}' has a truncated section table", name);
    }
    for (std::size_t index = 0; index < sectionCount; ++index)
    {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section, bytes.data() + sectionTable + index * sizeof(section),
                    sizeof(section));
        if (section.VirtualAddress >= imageSize)
        {
            continue;
        }
        // Uninitialized data has no bytes in the file; VirtualAlloc already zeroed it.
        const std::size_t rawSize =
            std::min<std::size_t>(section.SizeOfRawData, imageSize - section.VirtualAddress);
        if (rawSize == 0)
        {
            continue;
        }
        if (!fits(section.PointerToRawData, rawSize))
        {
            return MakeError(ErrorCode::Parse, "section {} of '{}' lies past the end of the file",
                             index, name);
        }
        std::memcpy(image + section.VirtualAddress, bytes.data() + section.PointerToRawData,
                    rawSize);
    }

    std::optional<Module> module = Module::FromImage(memory.get(), file);
    if (!module)
    {
        return MakeError(ErrorCode::Parse, "'{}' could not be read back after layout", name);
    }
    return ImageFile{std::move(memory), std::move(*module)};
}

Result<ImageFile> ImageFile::Capture(std::wstring_view processName)
{
    const std::string name = util::ToUtf8(std::filesystem::path{processName});
    const HANDLE processes = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE)
    {
        return MakeError(ErrorCode::Unavailable, "cannot list running processes");
    }
    PROCESSENTRY32W process{.dwSize = sizeof(PROCESSENTRY32W)};
    std::optional<DWORD> processId;
    for (BOOL more = ::Process32FirstW(processes, &process); more != FALSE;
         more = ::Process32NextW(processes, &process))
    {
        if (util::EqualsIgnoreCase(util::ToUtf8(std::filesystem::path{process.szExeFile}), name))
        {
            processId = process.th32ProcessID;
            break;
        }
    }
    ::CloseHandle(processes);
    if (!processId)
    {
        return MakeError(ErrorCode::NotFound, "no running process is called '{}'", name);
    }

    const HANDLE modules =
        ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, *processId);
    if (modules == INVALID_HANDLE_VALUE)
    {
        return MakeError(
            ErrorCode::AccessDenied,
            "cannot list the modules of '{}' (process {}); run elevated if the game is", name,
            *processId);
    }
    MODULEENTRY32W entry{.dwSize = sizeof(MODULEENTRY32W)};
    const bool found = ::Module32FirstW(modules, &entry) != FALSE; // the first one is the exe
    ::CloseHandle(modules);
    if (!found)
    {
        return MakeError(ErrorCode::NotFound, "'{}' reports no main module", name);
    }

    const HANDLE target =
        ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, *processId);
    if (target == nullptr)
    {
        return MakeError(ErrorCode::AccessDenied, "cannot open '{}' (process {}) for reading", name,
                         *processId);
    }

    const std::size_t imageSize = entry.modBaseSize;
    std::unique_ptr<void, VirtualFreeDeleter> memory{
        ::VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    if (!memory)
    {
        ::CloseHandle(target);
        return MakeError(ErrorCode::Unavailable, "cannot reserve {} bytes to copy '{}'", imageSize,
                         name);
    }

    // Page by page, so one guard or no-access page costs that page rather than the whole copy.
    constexpr std::size_t kPageBytes = 0x1000;
    std::size_t copiedBytes = 0;
    for (std::size_t offset = 0; offset < imageSize; offset += kPageBytes)
    {
        const std::size_t size = std::min(kPageBytes, imageSize - offset);
        SIZE_T read = 0;
        if (::ReadProcessMemory(target, entry.modBaseAddr + offset,
                                static_cast<char*>(memory.get()) + offset, size, &read) != FALSE)
        {
            copiedBytes += read;
        }
    }
    ::CloseHandle(target);

    std::optional<Module> module =
        Module::FromImage(memory.get(), std::filesystem::path{entry.szExePath});
    if (copiedBytes == 0 || !module)
    {
        return MakeError(ErrorCode::Parse, "the image of '{}' could not be read", name);
    }
    return ImageFile{std::move(memory), std::move(*module)};
}
} // namespace spl::memory
