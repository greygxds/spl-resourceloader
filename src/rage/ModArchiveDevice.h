#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spl::rage
{
/// A file the way a rage::fiDevice has to hand it to the game.
struct DeviceFileInfo
{
    /// What the length queries answer: kLargeSizeMarker for a large resource.
    uint64_t lengthBytes = 0;
    uint64_t fileTime = 0; ///< FILETIME ticks; never 0, which some game code takes as "none"

    /// A compiled resource's header: what GetResourceVersion answers.
    struct ResourceVersion
    {
        int32_t version = 0;
        uint32_t virtualFlags = 0;
        uint32_t physicalFlags = 0;
    };
    std::optional<ResourceVersion> resource;
};

/// The bytes reads are served from, kept alive by owned when they are not the source's own.
struct DeviceFileBytes
{
    std::span<const char> bytes;
    std::shared_ptr<const std::vector<char>> owned;
};

/// One child of a directory, for FindFirst and FindNext.
struct DeviceDirectoryEntry
{
    std::string name;
    bool isDirectory = false;
    uint64_t lengthBytes = 0;
    uint64_t fileTime = 0;
};

/// What a ModArchiveDevice serves. Paths are relative to the source's root, with forward slashes
/// and no leading slash ("" is the root), and compare case-insensitively. Called from the game's
/// streaming threads, so every method has to be safe to call concurrently.
class IDeviceFileSource
{
public:
    IDeviceFileSource() = default;
    IDeviceFileSource(const IDeviceFileSource&) = delete;
    IDeviceFileSource& operator=(const IDeviceFileSource&) = delete;
    virtual ~IDeviceFileSource() = default;

    [[nodiscard]] virtual std::optional<DeviceFileInfo> FindFile(std::string_view path) const = 0;
    [[nodiscard]] virtual bool IsDirectory(std::string_view path) const = 0;

    /// std::nullopt when the file is missing or its bytes cannot be produced.
    [[nodiscard]] virtual std::optional<DeviceFileBytes> OpenFile(std::string_view path) const = 0;

    /// The children of a directory, sorted case-insensitively. Empty for a missing one.
    [[nodiscard]] virtual std::vector<DeviceDirectoryEntry> List(std::string_view path) const = 0;
};

class ModArchiveDevice;

/// What the game sees: a vtable pointer where every rage::fiDevice has one, then our state.
struct ModArchiveDeviceObject
{
    const void* const* vtable; // +0x00
    ModArchiveDevice* owner;   // +0x08
};

/// A read-only rage::fiDevice over files that exist only inside mod archives, the part of FiveM's
/// vfs::RagePackfile7 and RageVFSDeviceAdapter (vfs-core/src/VFSRagePackfile7.cpp,
/// vfs-impl-rage/src/RageVFS.cpp) the game touches. Mounted at "splmods:/" over every mod, and
/// once per overlay at a game mount point ("common:/") over one mod's folder.
///
/// A path the game passes has its mount point ("anything:/") stripped and the device's folder put
/// in front before the source is asked. A large resource is served the way a packfile stores it:
/// its length is kLargeSizeMarker and its first 16 bytes are the size header, which is what the
/// streamer reads (see LooseResourceDevice). There is no real device behind this one, so every
/// slot the game may call is implemented; slots nobody is known to call answer "nothing" and are
/// recorded (GetUnexpectedSlots), so a build that uses one shows up in the log. Pure memory code,
/// like ForcedDevice, so it is tested without the game.
class ModArchiveDevice
{
public:
    static constexpr std::string_view kName = "SplModArchiveDevice";

    /// The size the streamer's 24-bit field uses to say "read the real size from the data".
    static constexpr uint64_t kLargeSizeMarker = 0xFFFFFF;

    /// source outlives the device. folder ("mycar/common") is put in front of every path; empty
    /// serves the source's root.
    ModArchiveDevice(const IDeviceFileSource& source, std::string folder);

    // The game holds a pointer to m_object, so the device must never move.
    ModArchiveDevice(const ModArchiveDevice&) = delete;
    ModArchiveDevice& operator=(const ModArchiveDevice&) = delete;
    ModArchiveDevice(ModArchiveDevice&&) = delete;
    ModArchiveDevice& operator=(ModArchiveDevice&&) = delete;
    ~ModArchiveDevice() = default;

    /// The pointer to mount.
    [[nodiscard]] void* GetGameDevice()
    {
        return &m_object;
    }

    [[nodiscard]] const IDeviceFileSource& GetSource() const
    {
        return m_source;
    }

    /// The source-relative path a game path names: "common:/data/x.meta" with folder
    /// "mycar/common" is "mycar/common/data/x.meta". Lower-cased, so it is also a lookup key.
    [[nodiscard]] std::string ToSourcePath(std::string_view gamePath) const;

    /// An open file or search, by the handle the game was given.
    struct OpenFile
    {
        DeviceFileBytes bytes;
        uint64_t lengthBytes = 0;
        uint64_t cursor = 0;
    };
    struct OpenSearch
    {
        std::vector<DeviceDirectoryEntry> entries;
        std::size_t next = 0;
    };

    [[nodiscard]] uint64_t AddFile(OpenFile file);
    /// Runs action on the open file under the lock. False when handle names none.
    template <typename TAction> bool WithFile(uint64_t handle, TAction&& action)
    {
        const std::lock_guard lock{m_mutex};
        const auto found = m_files.find(handle);
        if (found == m_files.end())
        {
            return false;
        }
        action(found->second);
        return true;
    }
    bool CloseFile(uint64_t handle);

    [[nodiscard]] uint64_t AddSearch(OpenSearch search);
    template <typename TAction> bool WithSearch(uint64_t handle, TAction&& action)
    {
        const std::lock_guard lock{m_mutex};
        const auto found = m_searches.find(handle);
        if (found == m_searches.end())
        {
            return false;
        }
        action(found->second);
        return true;
    }
    bool CloseSearch(uint64_t handle);

    /// How many files and searches are open, for the tests.
    [[nodiscard]] std::size_t CountOpenHandles() const;

    /// True the first time an unimplemented slot is called.
    [[nodiscard]] bool NoteUnexpectedSlot(std::size_t slot);

    /// The unimplemented slots the game has called, for diagnostics and the tests.
    [[nodiscard]] std::vector<std::size_t> GetUnexpectedSlots() const;

private:
    ModArchiveDeviceObject m_object;
    const IDeviceFileSource& m_source;
    std::string m_folder; ///< lower-case, no trailing slash

    mutable std::mutex m_mutex; ///< the streamer reads from several threads
    uint64_t m_nextHandle = 1;  ///< never kInvalidFileHandleRaw
    std::unordered_map<uint64_t, OpenFile> m_files;
    std::unordered_map<uint64_t, OpenSearch> m_searches;
    std::vector<std::size_t> m_unexpectedSlots;
};
} // namespace spl::rage
