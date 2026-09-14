#include "util/FileTree.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "core/Result.h"
#include "util/Strings.h"

namespace spl::util
{
const DiskFileTree& DiskFileTree::Instance()
{
    static const DiskFileTree instance;
    return instance;
}

std::optional<FileTreeStat> DiskFileTree::Stat(const std::filesystem::path& path) const
{
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status))
    {
        return std::nullopt;
    }
    FileTreeStat stat{.isDirectory = std::filesystem::is_directory(status)};
    if (!stat.isDirectory)
    {
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        stat.sizeBytes = error ? 0 : static_cast<uint64_t>(size);
    }
    std::error_code timeError;
    const std::filesystem::file_time_type written =
        std::filesystem::last_write_time(path, timeError);
    if (!timeError)
    {
        stat.lastWriteTime = written;
    }
    return stat;
}

Result<std::vector<FileTreeEntry>> DiskFileTree::List(const std::filesystem::path& directory) const
{
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    if (error)
    {
        return MakeError(ErrorCode::Io, "cannot read '{}': {}", ToUtf8Generic(directory),
                         error.message());
    }

    // The listing already carries each file's size and write time on Windows, so reading them
    // from the entry saves two filesystem calls per file.
    std::vector<FileTreeEntry> entries;
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        std::error_code entryError;
        if (entry.is_directory(entryError))
        {
            entries.push_back(FileTreeEntry{.path = entry.path(), .stat = {.isDirectory = true}});
            continue;
        }
        if (!entry.is_regular_file(entryError))
        {
            continue;
        }
        FileTreeEntry file{.path = entry.path()};
        const std::uintmax_t size = entry.file_size(entryError);
        if (!entryError)
        {
            file.stat.sizeBytes = static_cast<uint64_t>(size);
        }
        std::error_code timeError;
        const std::filesystem::file_time_type written = entry.last_write_time(timeError);
        if (!timeError)
        {
            file.stat.lastWriteTime = written;
        }
        entries.push_back(std::move(file));
    }
    return entries;
}

std::vector<std::string> DiskFileTree::ListFilesRecursive(const std::filesystem::path& root) const
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
    {
        return {};
    }
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error)
    {
        return {};
    }

    std::vector<std::string> files;
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError))
        {
            continue;
        }
        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), root, entryError);
        if (!entryError)
        {
            files.push_back(ToUtf8Generic(relative));
        }
    }
    return files;
}

Result<std::string> DiskFileTree::Read(const std::filesystem::path& file,
                                       std::size_t maxBytes) const
{
    std::ifstream stream{file, std::ios::binary};
    if (!stream)
    {
        return MakeError(ErrorCode::Io, "'{}' could not be opened", ToUtf8Generic(file));
    }
    if (maxBytes == std::numeric_limits<std::size_t>::max())
    {
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    }
    std::string contents(maxBytes, '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(stream.gcount()));
    return contents;
}
} // namespace spl::util
