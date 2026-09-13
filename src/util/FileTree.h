#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"

namespace spl::util
{
/// What a tree knows about one path.
struct FileTreeStat
{
    bool isDirectory = false;
    uint64_t sizeBytes = 0; ///< 0 for directories
    std::filesystem::file_time_type lastWriteTime{};
};

/// One child of a listed directory.
struct FileTreeEntry
{
    std::filesystem::path path; ///< the listed directory joined with the child's name
    FileTreeStat stat;
};

/// The files a resource is read from, by absolute path: a folder on disk, or a user mod served
/// from its archive under a root that exists only as a name. Everything the loader reads of a
/// resource goes through here, so the two look the same to the manifest, the glob and the scanner.
class IFileTree
{
public:
    IFileTree() = default;
    IFileTree(const IFileTree&) = delete;
    IFileTree& operator=(const IFileTree&) = delete;
    virtual ~IFileTree() = default;

    /// std::nullopt when nothing is at path.
    [[nodiscard]] virtual std::optional<FileTreeStat>
    Stat(const std::filesystem::path& path) const = 0;

    /// The files and directories directly under directory, in no particular order. An error when
    /// it cannot be listed, with a message ready to log.
    [[nodiscard]] virtual Result<std::vector<FileTreeEntry>>
    List(const std::filesystem::path& directory) const = 0;

    /// Every file under root at any depth, as root-relative paths with forward slashes, in no
    /// particular order. Empty when root cannot be read.
    [[nodiscard]] virtual std::vector<std::string>
    ListFilesRecursive(const std::filesystem::path& root) const = 0;

    /// The first maxBytes of a file (all of it by default).
    [[nodiscard]] virtual Result<std::string>
    Read(const std::filesystem::path& file,
         std::size_t maxBytes = std::numeric_limits<std::size_t>::max()) const = 0;
};

/// The real filesystem.
class DiskFileTree final : public IFileTree
{
public:
    /// Stateless, so one instance serves every folder resource.
    [[nodiscard]] static const DiskFileTree& Instance();

    [[nodiscard]] std::optional<FileTreeStat>
    Stat(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<std::vector<FileTreeEntry>>
    List(const std::filesystem::path& directory) const override;
    [[nodiscard]] std::vector<std::string>
    ListFilesRecursive(const std::filesystem::path& root) const override;
    [[nodiscard]] Result<std::string>
    Read(const std::filesystem::path& file,
         std::size_t maxBytes = std::numeric_limits<std::size_t>::max()) const override;
};
} // namespace spl::util
