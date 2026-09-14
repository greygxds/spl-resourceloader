#include "mods/ModFileTree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "core/Result.h"
#include "rage/ModArchiveDevice.h"
#include "rpf/RpfReader.h"
#include "rpf/RpfTypes.h"
#include "util/FileTree.h"
#include "util/Strings.h"

namespace spl::mods
{
namespace
{
/// The time the game is given when the archive's own is unknown: FiveM's, which is the README.txt
/// of the GTA1 EU release, because 0 means "none" to some game code
/// (vfs-impl-rage/src/RageVFS.cpp:225).
constexpr uint64_t kFallbackFileTime = 125213779100000000ULL;

constexpr std::size_t kRscHeaderBytes = rpf::RpfLayout::kHeaderSizeBytes;

[[nodiscard]] uint32_t ReadU32Le(const char* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] std::string ParentOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
}

[[nodiscard]] std::string NameOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view text)
{
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

/// "a\\b//c/" -> "a/b/c", the form lookups compare.
[[nodiscard]] std::string Normalize(std::string_view path)
{
    std::string normalized;
    normalized.reserve(path.size());
    for (const char c : path)
    {
        const char character = c == '\\' ? '/' : c;
        if (character == '/' && (normalized.empty() || normalized.back() == '/'))
        {
            continue;
        }
        normalized.push_back(character);
    }
    if (!normalized.empty() && normalized.back() == '/')
    {
        normalized.pop_back();
    }
    return normalized;
}
} // namespace

ModFileTree::ModFileTree(std::filesystem::path root,
                         std::vector<std::unique_ptr<rpf::RpfReader>> archives,
                         std::vector<ModFile> files, uint64_t fileTime)
    : m_root(std::move(root)), m_archives(std::move(archives)),
      m_fileTime(fileTime != 0 ? fileTime : kFallbackFileTime),
      m_lastWriteTime(std::filesystem::file_time_type::duration{static_cast<int64_t>(m_fileTime)})
{
    std::unordered_set<std::string> folders{""};
    for (ModFile& file : files)
    {
        file.path = Normalize(file.path);
        if (file.path.empty())
        {
            continue;
        }
        std::string key = util::ToLower(file.path);
        if (const auto existing = m_byLowerPath.find(key); existing != m_byLowerPath.end())
        {
            m_files[existing->second].source = std::move(file.source);
            continue;
        }

        m_children[util::ToLower(ParentOf(file.path))].push_back(Child{.name = NameOf(file.path)});
        for (std::string folder = ParentOf(file.path); !folder.empty(); folder = ParentOf(folder))
        {
            if (!folders.insert(util::ToLower(folder)).second)
            {
                break; // its parents are in already
            }
            m_children[util::ToLower(ParentOf(folder))].push_back(
                Child{.name = NameOf(folder), .isDirectory = true});
        }
        m_byLowerPath.emplace(std::move(key), m_files.size());
        m_files.push_back(std::move(file));
    }
    for (auto& [folder, children] : m_children)
    {
        std::ranges::sort(children, {},
                          [](const Child& child) { return util::ToLower(child.name); });
    }
}

const ModFile* ModFileTree::FindFile(std::string_view path) const
{
    const auto found = m_byLowerPath.find(util::ToLower(Normalize(path)));
    return found != m_byLowerPath.end() ? &m_files[found->second] : nullptr;
}

bool ModFileTree::IsDirectory(std::string_view path) const
{
    return m_children.contains(util::ToLower(Normalize(path)));
}

std::vector<rage::DeviceDirectoryEntry> ModFileTree::ListForGame(std::string_view path) const
{
    std::vector<rage::DeviceDirectoryEntry> entries;
    const std::string folder = Normalize(path);
    const auto found = m_children.find(util::ToLower(folder));
    if (found == m_children.end())
    {
        return entries;
    }
    for (const Child& child : found->second)
    {
        rage::DeviceDirectoryEntry entry{
            .name = child.name, .isDirectory = child.isDirectory, .fileTime = m_fileTime};
        if (!child.isDirectory)
        {
            const std::string childPath = folder.empty() ? child.name : folder + "/" + child.name;
            if (const ModFile* const file = FindFile(childPath))
            {
                if (const std::optional<rage::DeviceFileInfo> info = DescribeForGame(*file))
                {
                    entry.lengthBytes = info->lengthBytes;
                }
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::optional<rage::DeviceFileInfo> ModFileTree::DescribeForGame(const ModFile& file) const
{
    if (const auto* const generated = std::get_if<std::string>(&file.source))
    {
        return rage::DeviceFileInfo{.lengthBytes = generated->size(), .fileTime = m_fileTime};
    }
    const ArchiveEntry& entry = std::get<ArchiveEntry>(file.source);
    const Result<rpf::RpfReader::FileInfo> stat = entry.archive->Stat(entry.path);
    if (!stat)
    {
        return std::nullopt;
    }
    rage::DeviceFileInfo info{.lengthBytes = stat.GetValue().sizeBytes, .fileTime = m_fileTime};
    if (!stat.GetValue().isStored)
    {
        return info; // metadata: resources are never deflated by the tools that build packages
    }

    const Result<std::span<const char>> bytes = entry.archive->GetStoredBytes(entry.path);
    if (!bytes)
    {
        return std::nullopt;
    }
    info.lengthBytes = bytes.GetValue().size();
    if (stat.GetValue().isLargeResource)
    {
        // The streamer reads the real size from the data (see LooseResourceDevice), and the
        // flags from the table, as the header that would have them is not there.
        const uint32_t virtualFlags = stat.GetValue().virtualFlags;
        const uint32_t physicalFlags = stat.GetValue().physicalFlags;
        info.lengthBytes = rage::ModArchiveDevice::kLargeSizeMarker;
        info.resource = rage::DeviceFileInfo::ResourceVersion{
            .version = static_cast<int32_t>(((virtualFlags >> 28) << 4) | (physicalFlags >> 28)),
            .virtualFlags = virtualFlags,
            .physicalFlags = physicalFlags};
    }
    else if (bytes.GetValue().size() >= kRscHeaderBytes &&
             ReadU32Le(bytes.GetValue().data()) == rpf::RpfLayout::kRscMagic)
    {
        const char* const header = bytes.GetValue().data();
        info.resource = rage::DeviceFileInfo::ResourceVersion{
            .version = static_cast<int32_t>(ReadU32Le(header + 4)),
            .virtualFlags = ReadU32Le(header + 8),
            .physicalFlags = ReadU32Le(header + 12)};
    }
    return info;
}

std::optional<rage::DeviceFileBytes> ModFileTree::OpenForGame(const ModFile& file) const
{
    if (const auto* const generated = std::get_if<std::string>(&file.source))
    {
        return rage::DeviceFileBytes{.bytes = std::span<const char>{*generated}};
    }
    const ArchiveEntry& entry = std::get<ArchiveEntry>(file.source);
    if (const Result<std::span<const char>> stored = entry.archive->GetStoredBytes(entry.path))
    {
        return rage::DeviceFileBytes{.bytes = stored.GetValue()};
    }
    // Deflated: inflated whole, which only metadata files are.
    const Result<std::vector<std::byte>> inflated = entry.archive->ReadFile(entry.path);
    if (!inflated)
    {
        return std::nullopt;
    }
    auto owned = std::make_shared<std::vector<char>>(inflated.GetValue().size());
    std::memcpy(owned->data(), inflated.GetValue().data(), owned->size());
    const std::span<const char> view{*owned};
    return rage::DeviceFileBytes{.bytes = view, .owned = std::move(owned)};
}

std::optional<std::string> ModFileTree::ToRelative(const std::filesystem::path& path) const
{
    const std::filesystem::path relative = path.lexically_relative(m_root);
    if (relative.empty())
    {
        return std::nullopt;
    }
    if (relative == std::filesystem::path{"."})
    {
        return std::string{};
    }
    if (*relative.begin() == std::filesystem::path{".."})
    {
        return std::nullopt;
    }
    return Normalize(util::ToUtf8Generic(relative));
}

uint64_t ModFileTree::GetLoaderSize(const ModFile& file) const
{
    if (const auto* const generated = std::get_if<std::string>(&file.source))
    {
        return generated->size();
    }
    const ArchiveEntry& entry = std::get<ArchiveEntry>(file.source);
    const Result<rpf::RpfReader::FileInfo> stat = entry.archive->Stat(entry.path);
    return stat ? stat.GetValue().sizeBytes : 0;
}

std::optional<util::FileTreeStat> ModFileTree::Stat(const std::filesystem::path& path) const
{
    const std::optional<std::string> relative = ToRelative(path);
    if (!relative)
    {
        return std::nullopt;
    }
    if (IsDirectory(*relative))
    {
        return util::FileTreeStat{.isDirectory = true, .lastWriteTime = m_lastWriteTime};
    }
    const ModFile* const file = FindFile(*relative);
    if (file == nullptr)
    {
        return std::nullopt;
    }
    return util::FileTreeStat{.sizeBytes = GetLoaderSize(*file), .lastWriteTime = m_lastWriteTime};
}

Result<std::vector<util::FileTreeEntry>>
ModFileTree::List(const std::filesystem::path& directory) const
{
    const std::optional<std::string> relative = ToRelative(directory);
    const auto found = relative ? m_children.find(util::ToLower(*relative)) : m_children.end();
    if (found == m_children.end())
    {
        return MakeError(ErrorCode::NotFound, "cannot read '{}': the mod has no such folder",
                         util::ToUtf8Generic(directory));
    }
    std::vector<util::FileTreeEntry> entries;
    for (const Child& child : found->second)
    {
        util::FileTreeEntry entry{
            .path = directory / PathFromUtf8(child.name),
            .stat = {.isDirectory = child.isDirectory, .lastWriteTime = m_lastWriteTime}};
        if (!child.isDirectory)
        {
            const std::string childPath =
                relative->empty() ? child.name : *relative + "/" + child.name;
            if (const ModFile* const file = FindFile(childPath))
            {
                entry.stat.sizeBytes = GetLoaderSize(*file);
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<std::string> ModFileTree::ListFilesRecursive(const std::filesystem::path& root) const
{
    std::vector<std::string> found;
    const std::optional<std::string> relative = ToRelative(root);
    if (!relative || !IsDirectory(*relative))
    {
        return found;
    }
    const std::string prefix = relative->empty() ? std::string{} : util::ToLower(*relative) + "/";
    for (const ModFile& file : m_files)
    {
        if (util::ToLower(file.path).starts_with(prefix))
        {
            found.push_back(file.path.substr(prefix.size()));
        }
    }
    return found;
}

Result<std::string> ModFileTree::Read(const std::filesystem::path& file, std::size_t maxBytes) const
{
    const std::optional<std::string> relative = ToRelative(file);
    const ModFile* const found = relative ? FindFile(*relative) : nullptr;
    if (found == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "'{}' could not be opened",
                         util::ToUtf8Generic(file));
    }
    if (const auto* const generated = std::get_if<std::string>(&found->source))
    {
        return generated->substr(0, std::min(maxBytes, generated->size()));
    }
    const ArchiveEntry& entry = std::get<ArchiveEntry>(found->source);
    const Result<std::vector<std::byte>> bytes = entry.archive->ReadPrefix(entry.path, maxBytes);
    if (!bytes)
    {
        return MakeError(bytes.GetError().code, "'{}' could not be read ({})",
                         util::ToUtf8Generic(file), bytes.GetMessage());
    }
    return std::string{reinterpret_cast<const char*>(bytes.GetValue().data()),
                       bytes.GetValue().size()};
}
} // namespace spl::mods
