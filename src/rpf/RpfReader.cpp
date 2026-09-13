#include "rpf/RpfReader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <miniz.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/Win32.h"
#include "util/Strings.h"

namespace spl::rpf
{
namespace
{
/// Sanity cap on one inflated entry: real deflated entries are megabytes, so a gigabyte claim is
/// corruption or a bomb, not data. Stored entries are only bounded by the archive itself.
constexpr uint64_t kMaxDecompressedBytes = 1ULL << 30;

/// Little-endian 32-bit word; the format is LE and so is our target.
[[nodiscard]] uint32_t ReadU32Le(const char* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] uint64_t ReadU64Le(const char* bytes)
{
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

/// A name that starts at offset and terminates before the table ends.
[[nodiscard]] std::optional<std::string> ReadName(const std::string& names, uint32_t offset)
{
    if (offset >= names.size())
    {
        return std::nullopt;
    }
    const std::size_t end = names.find('\0', offset);
    if (end == std::string::npos)
    {
        return std::nullopt;
    }
    return names.substr(offset, end - offset);
}

[[nodiscard]] uint64_t DataOffset(const RpfEntryView& entry)
{
    return (static_cast<uint64_t>(RpfOffsetField(entry)) & RpfLayout::kDirectorySentinel) *
           RpfLayout::kSectorSizeBytes;
}

/// A mod archive on disk, mapped read-only for as long as a reader or a nested reader needs it.
class MappedFile final : public ArchiveBytes
{
public:
    [[nodiscard]] static Result<std::shared_ptr<const MappedFile>>
    Map(const std::filesystem::path& path)
    {
        auto mapped = std::make_shared<MappedFile>();
        mapped->m_file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapped->m_file == INVALID_HANDLE_VALUE)
        {
            return MakeError(ErrorCode::Io, "could not open '{}'", path.string());
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(mapped->m_file, &size) == 0)
        {
            return MakeError(ErrorCode::Io, "could not read the size of '{}'", path.string());
        }
        if (size.QuadPart < static_cast<LONGLONG>(RpfLayout::kHeaderSizeBytes))
        {
            return MakeError(ErrorCode::Parse, "'{}' is too small to be an RPF archive",
                             path.string());
        }
        mapped->m_mapping =
            ::CreateFileMappingW(mapped->m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapped->m_mapping == nullptr)
        {
            return MakeError(ErrorCode::Io, "could not map '{}'", path.string());
        }
        mapped->m_view = ::MapViewOfFile(mapped->m_mapping, FILE_MAP_READ, 0, 0, 0);
        if (mapped->m_view == nullptr)
        {
            return MakeError(ErrorCode::Io, "could not map '{}'", path.string());
        }
        mapped->m_size = static_cast<std::size_t>(size.QuadPart);
        return std::shared_ptr<const MappedFile>{std::move(mapped)};
    }

    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() override
    {
        if (m_view != nullptr)
        {
            ::UnmapViewOfFile(m_view);
        }
        if (m_mapping != nullptr)
        {
            ::CloseHandle(m_mapping);
        }
        if (m_file != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_file);
        }
    }

    [[nodiscard]] std::span<const char> GetBytes() const override
    {
        return {static_cast<const char*>(m_view), m_size};
    }

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
    LPVOID m_view = nullptr;
    std::size_t m_size = 0;
};

class OwnedBytes final : public ArchiveBytes
{
public:
    explicit OwnedBytes(std::string bytes) : m_bytes(std::move(bytes)) {}

    [[nodiscard]] std::span<const char> GetBytes() const override
    {
        return m_bytes;
    }

private:
    std::string m_bytes;
};

/// Validates the tree once at open: every child range in bounds, every name terminated, no
/// entry reachable twice (a corrupt table could cycle), and every file's on-disk bytes inside the
/// archive (truncated downloads fail here, not at read). A large resource's true size is only
/// known from its data, so its bytes are checked when it is read.
[[nodiscard]] Result<void> ValidateTree(const std::vector<RpfEntryView>& entries,
                                        const std::string& names, std::size_t archiveSizeBytes)
{
    if (entries.empty() || !RpfIsDirectory(entries[0]))
    {
        return MakeError(ErrorCode::Parse, "the RPF root entry is not a directory");
    }
    std::vector<bool> seen(entries.size(), false);
    std::vector<std::size_t> stack{0};
    seen[0] = true;
    while (!stack.empty())
    {
        const std::size_t index = stack.back();
        stack.pop_back();
        const RpfEntryView& entry = entries[index];
        if (!ReadName(names, RpfNameOffset(entry)))
        {
            return MakeError(ErrorCode::Parse, "entry {} names bytes outside the name table",
                             index);
        }
        if (!RpfIsDirectory(entry))
        {
            const uint64_t onDisk = RpfIsLargeResource(entry) ? RpfLayout::kHeaderSizeBytes
                                    : RpfIsStored(entry)      ? RpfStoredSize(entry)
                                                              : RpfDataSize(entry);
            if (DataOffset(entry) + onDisk > archiveSizeBytes)
            {
                return MakeError(ErrorCode::Parse, "entry {} claims bytes past the end of file",
                                 index);
            }
            continue;
        }
        const uint64_t first = entry.virtFlags;
        const uint64_t last = first + entry.physFlags;
        if (last > entries.size())
        {
            return MakeError(ErrorCode::Parse, "directory '{}' lists children outside the table",
                             *ReadName(names, RpfNameOffset(entry)));
        }
        for (uint64_t child = first; child < last; ++child)
        {
            if (seen[child])
            {
                return MakeError(ErrorCode::Parse, "entry {} is reachable twice", child);
            }
            seen[child] = true;
            stack.push_back(static_cast<std::size_t>(child));
        }
    }
    return {};
}
} // namespace

Result<RpfReader> RpfReader::Open(const std::filesystem::path& file)
{
    Result<std::shared_ptr<const MappedFile>> mapped = MappedFile::Map(file);
    if (!mapped)
    {
        return mapped.GetError();
    }
    const std::span<const char> bytes = mapped.GetValue()->GetBytes();
    return Parse(std::move(mapped.GetValue()), bytes, file);
}

Result<RpfReader> RpfReader::OpenFromBytes(std::string bytes, std::filesystem::path displayPath)
{
    auto owned = std::make_shared<const OwnedBytes>(std::move(bytes));
    const std::span<const char> view = owned->GetBytes();
    return Parse(std::move(owned), view, std::move(displayPath));
}

Result<RpfReader> RpfReader::OpenNested(std::string_view path) const
{
    Result<const RpfEntryView*> found = FindFile(path);
    if (!found)
    {
        return found.GetError();
    }
    const RpfEntryView& entry = *found.GetValue();
    const std::filesystem::path shown = m_path / std::string{path};
    if (!RpfIsStored(entry))
    {
        Result<std::vector<std::byte>> inflated = Inflate(path, entry);
        if (!inflated)
        {
            return inflated.GetError();
        }
        const std::vector<std::byte>& value = inflated.GetValue();
        return OpenFromBytes(std::string{reinterpret_cast<const char*>(value.data()), value.size()},
                             shown);
    }
    Result<std::span<const char>> located = Locate(path, entry);
    if (!located)
    {
        return located.GetError();
    }
    return Parse(m_storage, located.GetValue(), shown);
}

Result<RpfReader> RpfReader::Parse(std::shared_ptr<const ArchiveBytes> storage,
                                   std::span<const char> bytes, std::filesystem::path displayPath)
{
    const std::string shown = displayPath.string();
    if (bytes.size() < RpfLayout::kHeaderSizeBytes)
    {
        return MakeError(ErrorCode::Parse, "'{}' is too small to be an RPF archive", shown);
    }

    const RpfHeaderView header{.magic = ReadU32Le(bytes.data()),
                               .entryCount = ReadU32Le(bytes.data() + 4),
                               .nameLength = ReadU32Le(bytes.data() + 8),
                               .encryption = ReadU32Le(bytes.data() + 12)};
    if (header.magic != RpfLayout::kMagicRpf7)
    {
        return MakeError(ErrorCode::NotSupported, "'{}' is not an RPF7 archive", shown);
    }
    if (header.encryption != RpfLayout::kEncryptionOpen &&
        header.encryption != RpfLayout::kEncryptionSigned)
    {
        return MakeError(ErrorCode::NotSupported, "'{}' is encrypted; re-save it as OPEN in OpenIV",
                         shown);
    }
    if (header.entryCount == 0 || header.entryCount > RpfLayout::kMaxEntryCount ||
        header.nameLength > RpfLayout::kMaxNameLength)
    {
        return MakeError(ErrorCode::Parse, "'{}' has an impossible table size", shown);
    }

    const std::size_t tableBytes =
        static_cast<std::size_t>(header.entryCount) * RpfLayout::kEntrySizeBytes;
    const std::size_t namesEnd = RpfLayout::kHeaderSizeBytes + tableBytes + header.nameLength;
    if (namesEnd > bytes.size())
    {
        return MakeError(ErrorCode::Parse, "'{}' ends inside its entry table", shown);
    }

    RpfReader reader;
    reader.m_path = std::move(displayPath);
    reader.m_entries.reserve(header.entryCount);
    const char* table = bytes.data() + RpfLayout::kHeaderSizeBytes;
    for (uint32_t index = 0; index < header.entryCount; ++index)
    {
        const char* row = table + index * RpfLayout::kEntrySizeBytes;
        reader.m_entries.push_back(RpfEntryView{.packed = ReadU64Le(row),
                                                .virtFlags = ReadU32Le(row + 8),
                                                .physFlags = ReadU32Le(row + 12)});
    }
    reader.m_names.assign(table + tableBytes, header.nameLength);

    if (const Result<void>& valid = ValidateTree(reader.m_entries, reader.m_names, bytes.size());
        !valid)
    {
        return valid.GetError();
    }
    reader.m_storage = std::move(storage);
    reader.m_bytes = bytes;
    reader.BuildLookup();
    return reader;
}

void RpfReader::BuildLookup()
{
    std::vector<std::pair<std::size_t, std::string>> stack{{0, ""}};
    while (!stack.empty())
    {
        const auto [index, parent] = stack.back();
        stack.pop_back();
        const RpfEntryView& entry = m_entries[index];
        const std::optional<std::string> name = ReadName(m_names, RpfNameOffset(entry));
        if (!name || index == 0)
        {
            if (name)
            {
                for (uint32_t child = entry.physFlags; child-- > 0;)
                {
                    stack.emplace_back(entry.virtFlags + child, "");
                }
            }
            continue;
        }
        const std::string path = parent.empty() ? *name : parent + "/" + *name;
        m_byPath.emplace(path, index);
        m_byLowerPath.try_emplace(util::ToLower(path), index);
        if (RpfIsDirectory(entry))
        {
            for (uint32_t offset = entry.physFlags; offset-- > 0;)
            {
                stack.emplace_back(entry.virtFlags + offset, path);
            }
        }
    }
}

Result<const RpfEntryView*> RpfReader::FindFile(std::string_view path) const
{
    // Archive hierarchy spells separators forward; mod assemblies spell them backslash.
    std::string normalized{path};
    std::ranges::replace(normalized, '\\', '/');
    const std::size_t stripped = normalized.find_first_not_of('/');
    const std::string relative = (stripped == std::string::npos) ? "" : normalized.substr(stripped);
    std::size_t index = m_entries.size();
    if (const auto exact = m_byPath.find(relative); exact != m_byPath.end())
    {
        index = exact->second;
    }
    else if (const auto folded = m_byLowerPath.find(util::ToLower(relative));
             folded != m_byLowerPath.end())
    {
        index = folded->second;
    }
    else
    {
        return MakeError(ErrorCode::NotFound, "'{}' is not in '{}'", relative, m_path.string());
    }

    const RpfEntryView& entry = m_entries[index];
    if (RpfIsDirectory(entry))
    {
        return MakeError(ErrorCode::InvalidArgument, "'{}' in '{}' is a directory", relative,
                         m_path.string());
    }
    return &entry;
}

Result<std::span<const char>> RpfReader::Locate(std::string_view path,
                                                const RpfEntryView& entry) const
{
    const uint64_t begin = DataOffset(entry);
    uint64_t length = RpfIsStored(entry) ? RpfStoredSize(entry) : RpfDataSize(entry);
    if (RpfIsLargeResource(entry))
    {
        length = RpfLargeResourceSize(std::span<const char, RpfLayout::kHeaderSizeBytes>{
            m_bytes.data() + begin, RpfLayout::kHeaderSizeBytes});
        if (length < RpfLayout::kHeaderSizeBytes)
        {
            return MakeError(ErrorCode::Parse, "'{}' in '{}' has a corrupt size header", path,
                             m_path.string());
        }
    }
    if (begin + length > m_bytes.size())
    {
        return MakeError(ErrorCode::Parse, "'{}' in '{}' claims bytes past the end of file", path,
                         m_path.string());
    }
    return std::span{m_bytes.data() + begin, static_cast<std::size_t>(length)};
}

Result<std::vector<std::byte>> RpfReader::ReadFile(std::string_view path) const
{
    Result<const RpfEntryView*> found = FindFile(path);
    if (!found)
    {
        return found.GetError();
    }
    const RpfEntryView& entry = *found.GetValue();
    if (!RpfIsStored(entry))
    {
        return Inflate(path, entry);
    }
    Result<std::span<const char>> located = Locate(path, entry);
    if (!located)
    {
        return located.GetError();
    }
    const std::span<const char> bytes = located.GetValue();
    std::vector<std::byte> contents(bytes.size());
    std::memcpy(contents.data(), bytes.data(), bytes.size());
    if (RpfIsLargeResource(entry))
    {
        // The size header takes the RSC7 header's place; the entry still has its flags.
        const std::array<char, RpfLayout::kHeaderSizeBytes> header =
            MakeRscHeader(entry.virtFlags, entry.physFlags);
        std::memcpy(contents.data(), header.data(), header.size());
    }
    return contents;
}

Result<std::vector<std::byte>> RpfReader::Inflate(std::string_view path,
                                                  const RpfEntryView& entry) const
{
    // Raw deflate, no zlib header or checksum (FiveM VFSRagePackfile7.cpp:279: -15).
    if (entry.virtFlags == 0 || entry.virtFlags > kMaxDecompressedBytes)
    {
        return MakeError(ErrorCode::Parse, "'{}' in '{}' claims an impossible size", path,
                         m_path.string());
    }
    Result<std::span<const char>> located = Locate(path, entry);
    if (!located)
    {
        return located.GetError();
    }
    std::vector<std::byte> contents(static_cast<std::size_t>(entry.virtFlags));
    const size_t written = tinfl_decompress_mem_to_mem(
        contents.data(), contents.size(), located.GetValue().data(), located.GetValue().size(), 0);
    if (written != contents.size())
    {
        return MakeError(ErrorCode::Parse, "'{}' in '{}' failed to inflate", path, m_path.string());
    }
    return contents;
}

std::vector<RpfReader::EntryInfo> RpfReader::Enumerate() const
{
    std::vector<EntryInfo> found;
    if (m_entries.empty())
    {
        return found;
    }
    std::vector<std::pair<std::size_t, std::string>> stack{{0, ""}};
    while (!stack.empty())
    {
        const auto [index, parent] = stack.back();
        stack.pop_back();
        const RpfEntryView& entry = m_entries[index];
        const std::optional<std::string> name = ReadName(m_names, RpfNameOffset(entry));
        if (!name)
        {
            continue; // validated at open; never happens
        }
        if (index == 0)
        {
            for (uint32_t child = entry.physFlags; child-- > 0;)
            {
                stack.emplace_back(entry.virtFlags + child, "");
            }
            continue;
        }
        const std::string path = parent.empty() ? *name : parent + "/" + *name;
        if (RpfIsDirectory(entry))
        {
            found.push_back(EntryInfo{.path = path, .isDirectory = true});
            for (uint32_t offset = entry.physFlags; offset-- > 0;)
            {
                stack.emplace_back(entry.virtFlags + offset, path);
            }
            continue;
        }
        uint64_t sizeBytes = RpfIsStored(entry) ? RpfStoredSize(entry) : entry.virtFlags;
        if (RpfIsLargeResource(entry))
        {
            const Result<std::span<const char>> located = Locate(path, entry);
            sizeBytes = located ? located.GetValue().size() : 0;
        }
        found.push_back(EntryInfo{.path = path, .sizeBytes = sizeBytes});
    }
    return found;
}
} // namespace spl::rpf
