#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"
#include "rage/LooseResourceDevice.h"
#include "rage/ModArchiveDevice.h"

namespace spl::rage
{
/// Access to RAGE's device layer: which device serves a path, what it says about a file, and
/// the devices we add ourselves — an fiDeviceRelative over the resources folder at
/// "splres:/", plus a ModArchiveDevice serving the user mods from their archives at
/// "splmods:/", so the game can open files from either by VFS path.
class FileDeviceInterface
{
public:
    [[nodiscard]] Result<void> Initialize(const GameAddresses& addresses);

    /// Proves that the game's own mounts answer, and that what comes back looks like a
    /// device object rather than a stale pointer.
    [[nodiscard]] Result<void> Verify(const memory::Module& image) const;

    /// Constructs an fiDeviceRelative over root and mounts it at "splres:/". Idempotent: the
    /// device is created once per process and the game keeps the pointer for good, so a
    /// second call with the same root is a no-op and a different root is an error.
    [[nodiscard]] Result<void> MountResourcesRoot(const std::filesystem::path& root);

    /// Mounts a ModArchiveDevice over mods at "splmods:/", so every adopted mod's files are
    /// reachable by VFS path under root, the folder their resources are rooted at (which does not
    /// exist on disk). mods must outlive the process's use of the device. Only called when at
    /// least one mod was adopted; a second call with the same root is a no-op.
    [[nodiscard]] Result<void> MountModsRoot(const std::filesystem::path& root,
                                             const IDeviceFileSource& mods);

    /// Mounts a ModArchiveDevice over one mod folder of mods ("mycar/common") at a game mount
    /// point ("common:/"), so the files in it replace the game's own, as FiveM does for a mod's
    /// common/ and platform/ (ModVFSDevice.cpp:354-389). The game's device stays mounted
    /// underneath. True when probeFile now resolves to our device; false when the game's device
    /// still answers first, which is logged by the caller.
    [[nodiscard]] Result<bool> MountOverlay(const IDeviceFileSource& mods, std::string folder,
                                            std::string_view mountPoint,
                                            std::string_view probeFile);

    /// Every vtable slot of our mod devices that the game called although nothing was known to
    /// call it, for the log. Empty on the builds the device was checked against.
    [[nodiscard]] std::vector<std::size_t> GetUnexpectedModDeviceSlots() const;

    /// True when path ("common:/data/visualsettings.dat") is served by one of the overlays
    /// MountOverlay put in, and not by a game device mounted or sorted in front since.
    [[nodiscard]] bool IsServedByOverlay(std::string_view path) const;

    [[nodiscard]] bool IsResourcesRootMounted() const
    {
        return m_mountedDevice != nullptr;
    }

    /// The VFS path for a file under either mounted root, or std::nullopt when it is
    /// under neither or nothing is mounted yet.
    [[nodiscard]] std::optional<std::string>
    ToVfsPath(const std::filesystem::path& absoluteFile) const;

    /// The rage::fiDevice that serves path ("platform:/"), or nullptr when none does. The
    /// device is owned by the game and outlives the call; we never free it.
    [[nodiscard]] void* GetDevice(std::string_view path, bool allowRoot) const;

    /// The device's attributes for path, or std::nullopt when the file does not exist.
    [[nodiscard]] std::optional<uint32_t> GetFileAttributes(void* device,
                                                            std::string_view path) const;

    /// The device's own name, for diagnostics. Empty when the call could not be made.
    [[nodiscard]] std::string GetDeviceName(void* device) const;

    /// True when the global mount and unmount calls resolved, which .ymf loading needs.
    [[nodiscard]] bool CanMountGlobally() const
    {
        return m_mountGlobal != 0 && m_unmount != 0;
    }

    /// Mounts a device the caller keeps alive at mountPoint. False when the game refused or the
    /// call faulted.
    [[nodiscard]] bool MountGlobal(std::string_view mountPoint, void* device, bool allowRoot) const;

    /// Removes whatever is mounted at mountPoint. False when the call could not be made.
    bool Unmount(std::string_view mountPoint) const;

    /// True when the first byteCount bytes of path open and read through the device stack.
    [[nodiscard]] bool CanReadFile(std::string_view path, std::size_t byteCount) const;

private:
    /// Places an fiDeviceRelative in the process-lifetime storage and points it at root.
    /// Each mount owns its storage: the game keeps the pointer for the whole process.
    [[nodiscard]] Result<void*> CreateRelativeDevice(void* storage, std::size_t storageBytes,
                                                     bool& constructed,
                                                     const std::string& root) const;

    /// Mounts a LooseResourceDevice over relativeDevice at mountPoint, so resources larger than
    /// 16 MiB stream, and returns what now answers for the mount point: the new device, or
    /// relativeDevice alone when it could not be mounted (which is logged).
    [[nodiscard]] void* PutLooseResourceDeviceInFront(void* relativeDevice,
                                                      const std::string& mountPoint);

    /// Mounts a device of ours with MountGlobal and proves the mount point now resolves to it.
    [[nodiscard]] Result<void> MountAndVerify(std::string_view mountPoint, void* device,
                                              std::string_view rootForLog);

    uintptr_t m_getDevice = 0;
    uintptr_t m_relativeVftable = 0;
    uintptr_t m_relativeSetPath = 0;
    uintptr_t m_relativeMount = 0;
    uintptr_t m_mountGlobal = 0; ///< optional: .ymf loading only
    uintptr_t m_unmount = 0;     ///< optional: .ymf loading only

    std::filesystem::path m_resourcesRoot;
    void* m_mountedDevice = nullptr; ///< non-owning: the game keeps it until the process ends
    std::filesystem::path m_modsRoot;
    void* m_modsDevice = nullptr; ///< the game object of the root mod device, once mounted

    /// Every mod device, root and overlays, kept for the process: the game's mount table keeps
    /// pointers to them.
    std::vector<std::unique_ptr<ModArchiveDevice>> m_modDevices;
    std::vector<void*> m_overlays; ///< the devices answering for each overlay, non-owning

    /// In front of every mount of ours; kept for the process, like the devices they front.
    std::vector<std::unique_ptr<LooseResourceDevice>> m_looseResourceDevices;
};
} // namespace spl::rage
