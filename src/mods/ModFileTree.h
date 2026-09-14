#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/Result.h"
#include "rage/ModArchiveDevice.h"
#include "rpf/RpfReader.h"
#include "util/FileTree.h"

namespace spl::mods
{
/// A file inside one of a mod's archives.
struct ArchiveEntry
{
    const rpf::RpfReader* archive = nullptr; ///< owned by the ModFileTree the file is in
    std::string path;                        ///< archive-relative, as RpfReader looks it up
};

/// One file of a mod's layout: where it appears, and what it is read from.
struct ModFile
{
    std::string path; ///< mod-relative with forward slashes: "stream/car.ytd", "fxmanifest.lua"
    std::variant<ArchiveEntry, std::string> source; ///< an archive entry, or generated contents
};

/// A mod's files, laid out the way a resource folder would hold them, read straight from the
/// mod's archives. Two views of the same files:
/// - the loader's, through util::IFileTree by absolute path under GetRoot(), a folder that does
///   not exist on disk: files read back as they would be on disk, large resources with RSC7;
/// - the game's, through DescribeForGame and OpenForGame: files as the archive stores them.
///
/// Immutable once built, so both views are safe to use from any thread.
class ModFileTree final : public util::IFileTree
{
public:
    /// archives keeps every reader a file points into alive. A path given twice (compared
    /// case-insensitively) keeps its first spelling and its last source, as a second write of the
    /// same file on disk would.
    ModFileTree(std::filesystem::path root, std::vector<std::unique_ptr<rpf::RpfReader>> archives,
                std::vector<ModFile> files, uint64_t fileTime);

    [[nodiscard]] const std::filesystem::path& GetRoot() const
    {
        return m_root;
    }

    [[nodiscard]] const std::vector<ModFile>& GetFiles() const
    {
        return m_files;
    }

    /// The file at a mod-relative path, or nullptr. Case-insensitive.
    [[nodiscard]] const ModFile* FindFile(std::string_view path) const;

    /// True for "" and for every folder a file sits under. Case-insensitive.
    [[nodiscard]] bool IsDirectory(std::string_view path) const;

    /// The children of a mod-relative folder for the game, sorted case-insensitively.
    [[nodiscard]] std::vector<rage::DeviceDirectoryEntry> ListForGame(std::string_view path) const;

    [[nodiscard]] std::optional<rage::DeviceFileInfo> DescribeForGame(const ModFile& file) const;
    [[nodiscard]] std::optional<rage::DeviceFileBytes> OpenForGame(const ModFile& file) const;

    [[nodiscard]] std::optional<util::FileTreeStat>
    Stat(const std::filesystem::path& path) const override;
    [[nodiscard]] Result<std::vector<util::FileTreeEntry>>
    List(const std::filesystem::path& directory) const override;
    [[nodiscard]] std::vector<std::string>
    ListFilesRecursive(const std::filesystem::path& root) const override;
    [[nodiscard]] Result<std::string>
    Read(const std::filesystem::path& file,
         std::size_t maxBytes = std::numeric_limits<std::size_t>::max()) const override;

private:
    struct Child
    {
        std::string name;
        bool isDirectory = false;
    };

    /// The mod-relative path of an absolute one, or std::nullopt when it is not under the root.
    [[nodiscard]] std::optional<std::string> ToRelative(const std::filesystem::path& path) const;

    /// The loader's size of a file: what reading it gives.
    [[nodiscard]] uint64_t GetLoaderSize(const ModFile& file) const;

    std::filesystem::path m_root;
    std::vector<std::unique_ptr<rpf::RpfReader>> m_archives;
    std::vector<ModFile> m_files;
    uint64_t m_fileTime = 0;
    std::filesystem::file_time_type m_lastWriteTime{};
    std::unordered_map<std::string, std::size_t> m_byLowerPath;
    std::unordered_map<std::string, std::vector<Child>> m_children; ///< by lower-case folder
};
} // namespace spl::mods
