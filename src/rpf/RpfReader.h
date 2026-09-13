#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Result.h"
#include "rpf/RpfTypes.h"

namespace spl::rpf
{
/// The bytes an archive reads from: a mapped file, or a buffer it owns.
class ArchiveBytes
{
public:
    ArchiveBytes() = default;
    ArchiveBytes(const ArchiveBytes&) = delete;
    ArchiveBytes& operator=(const ArchiveBytes&) = delete;
    virtual ~ArchiveBytes() = default;

    [[nodiscard]] virtual std::span<const char> GetBytes() const = 0;
};

/// A read-only RPF7 archive: OPEN, or CFXP read without checking its signature, the way FiveM
/// opens one when nothing validates it (vfs-core/src/VFSRagePackfile7.cpp:90). Files on disk
/// are mapped rather than read, so a multi-gigabyte mod costs address space, not memory, and a
/// nested archive stored uncompressed is read in place. Lookup mirrors FiveM's reader: exact
/// descent from the root, no name hashing (VFSRagePackfile7.cpp:162-191).
class RpfReader
{
public:
    /// One node in the archive, with a forward-slash path relative to the root.
    struct EntryInfo
    {
        std::string path; ///< "content/file.ytd"; directories have no trailing slash
        bool isDirectory = false;
        uint64_t sizeBytes = 0; ///< what ReadFile returns; 0 for directories
    };

    /// Opens and validates file. NotSupported means it is not an archive we read
    /// (wrong magic, encrypted, RPF2); Parse means it claims to be one but is corrupt;
    /// Io means it could not be read.
    [[nodiscard]] static Result<RpfReader> Open(const std::filesystem::path& file);

    /// Opens an archive from memory. displayPath names the archive in diagnostics.
    [[nodiscard]] static Result<RpfReader> OpenFromBytes(std::string bytes,
                                                         std::filesystem::path displayPath);

    /// Opens the archive stored at path inside this one: in place when it is stored, inflated
    /// into memory when it is compressed. The nested reader keeps the bytes alive on its own.
    [[nodiscard]] Result<RpfReader> OpenNested(std::string_view path) const;

    /// Every node under the root, depth-first, in entry order.
    [[nodiscard]] std::vector<EntryInfo> Enumerate() const;

    /// The bytes of a file at a forward-slash path (leading slashes ignored), as it would be on
    /// disk: stored entries sliced, deflated entries inflated (raw deflate, FiveM
    /// VFSRagePackfile7.cpp:279), and a resource too large for the entry's size field given its
    /// RSC7 header back. Lookup is exact first, then case-insensitive (VFSRagePackfile7.cpp:180).
    /// NotFound for missing paths, InvalidArgument for directories, Parse for corrupt data.
    [[nodiscard]] Result<std::vector<std::byte>> ReadFile(std::string_view path) const;

    /// Whether a file (not a directory) is at path, looked up the way ReadFile looks it up.
    [[nodiscard]] bool Contains(std::string_view path) const
    {
        return FindFile(path).HasValue();
    }

    [[nodiscard]] const std::filesystem::path& GetPath() const
    {
        return m_path;
    }

    /// How many entries the table holds, root included.
    [[nodiscard]] std::size_t GetEntryCount() const
    {
        return m_entries.size();
    }

private:
    /// Parses and validates the table of the archive occupying bytes.
    [[nodiscard]] static Result<RpfReader> Parse(std::shared_ptr<const ArchiveBytes> storage,
                                                 std::span<const char> bytes,
                                                 std::filesystem::path displayPath);

    void BuildLookup();

    /// The entry at a path, or an error naming what went wrong. Never a directory.
    [[nodiscard]] Result<const RpfEntryView*> FindFile(std::string_view path) const;

    /// Where a file entry's bytes are, as stored in the archive: for a large resource, the
    /// size header included; for a deflated file, the compressed bytes.
    [[nodiscard]] Result<std::span<const char>> Locate(std::string_view path,
                                                       const RpfEntryView& entry) const;

    [[nodiscard]] Result<std::vector<std::byte>> Inflate(std::string_view path,
                                                         const RpfEntryView& entry) const;

    std::filesystem::path m_path;
    std::shared_ptr<const ArchiveBytes> m_storage; ///< keeps m_bytes valid
    std::span<const char> m_bytes;                 ///< this archive, from its header on
    std::vector<RpfEntryView> m_entries;
    std::string m_names;
    std::unordered_map<std::string, std::size_t> m_byPath;      ///< exact full paths
    std::unordered_map<std::string, std::size_t> m_byLowerPath; ///< first index per lower path
};
} // namespace spl::rpf
