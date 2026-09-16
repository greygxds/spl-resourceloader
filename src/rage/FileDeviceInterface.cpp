#include "rage/FileDeviceInterface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/LooseResourceDevice.h"
#include "rage/SafeCall.h"
#include "rage/VfsPath.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
using GetDeviceFn = void* (*)(const char* path, bool allowRoot);
using SetPathFn = void (*)(void* self, const char* root, bool allowRoot, void* baseDevice);
using MountFn = void (*)(void* self, const char* mountPoint, bool allowRoot);
using MountGlobalFn = bool (*)(const char* mountPoint, void* device, bool allowRoot);
using UnmountFn = void (*)(const char* mountPoint);

/// Mount points every build has. They are the cheapest proof that the device layer is up and
/// that our GetDevice address is the real one.
constexpr std::array<std::string_view, 2> kWellKnownMounts = {"platform:/", "common:/"};

/// Storage for the fiDeviceRelatives we construct. The game keeps the pointers in its
/// mount table for the rest of the process, so the objects must never move and must never
/// be freed; static storage is how you say that. rage::fiDeviceRelative is 8 (vptr) + 272
/// bytes.
constexpr std::size_t kRelativeDeviceBytes = 0x200;
alignas(16) std::byte g_resourcesDeviceStorage[kRelativeDeviceBytes];
bool g_resourcesDeviceConstructed = false;

/// FiveM's constructor writes the vtable and then m_pad[256] = '\0'
/// (rage-device-five/src/fiDeviceClasses.cpp:13-18).
constexpr std::ptrdiff_t kRelativePadTerminator = 8 + 256;
} // namespace

Result<void> FileDeviceInterface::Initialize(const GameAddresses& addresses)
{
    m_getDevice = addresses.fiDeviceGetDevice;
    m_relativeVftable = addresses.fiDeviceRelativeVftable;
    m_relativeSetPath = addresses.fiDeviceRelativeSetPath;
    m_relativeMount = addresses.fiDeviceRelativeMount;
    m_mountGlobal = addresses.fiDeviceMountGlobal;
    m_unmount = addresses.fiDeviceUnmount;

    if (m_getDevice == 0 || m_relativeVftable == 0 || m_relativeSetPath == 0 ||
        m_relativeMount == 0)
    {
        return MakeError(ErrorCode::Unavailable, "the file-device addresses did not resolve");
    }
    return {};
}

void* FileDeviceInterface::GetDevice(std::string_view path, bool allowRoot) const
{
    if (m_getDevice == 0 || HasFaulted())
    {
        return nullptr;
    }

    const std::string terminated(path);
    const auto getDevice = reinterpret_cast<GetDeviceFn>(m_getDevice);
    const std::optional<void*> device =
        SafeCall("fiDevice::GetDevice", [&] { return getDevice(terminated.c_str(), allowRoot); });
    return device.value_or(nullptr);
}

std::optional<uint32_t> FileDeviceInterface::GetFileAttributes(void* device,
                                                               std::string_view path) const
{
    using GetFileAttributesFn = uint32_t (*)(void* self, const char* path);
    if (device == nullptr || HasFaulted())
    {
        return std::nullopt;
    }

    const std::string terminated(path);
    const auto self = reinterpret_cast<uintptr_t>(device);
    const std::optional<uint32_t> attributes =
        SafeCall("fiDevice::GetFileAttributes",
                 [&]
                 {
                     const auto getAttributes = GetVirtualFunction<GetFileAttributesFn>(
                         self, FileDeviceLayout::kSlotGetFileAttributes);
                     return getAttributes(device, terminated.c_str());
                 });
    if (!attributes || *attributes == kInvalidFileAttributesRaw)
    {
        return std::nullopt;
    }
    return attributes;
}

std::string FileDeviceInterface::GetDeviceName(void* device) const
{
    using GetNameFn = const char* (*)(void* self);
    if (device == nullptr || HasFaulted())
    {
        return {};
    }

    const auto self = reinterpret_cast<uintptr_t>(device);
    const std::optional<const char*> name =
        SafeCall("fiDevice::GetName",
                 [&]
                 {
                     const auto getName =
                         GetVirtualFunction<GetNameFn>(self, FileDeviceLayout::kSlotGetName);
                     return getName(device);
                 });
    if (!name || *name == nullptr)
    {
        return {};
    }
    return std::string(*name);
}

bool FileDeviceInterface::MountGlobal(std::string_view mountPoint, void* device,
                                      bool allowRoot) const
{
    if (m_mountGlobal == 0 || device == nullptr || HasFaulted())
    {
        return false;
    }

    const std::string terminated(mountPoint);
    const auto mountGlobal = reinterpret_cast<MountGlobalFn>(m_mountGlobal);
    const std::optional<bool> mounted =
        SafeCall("fiDevice::MountGlobal",
                 [&] { return mountGlobal(terminated.c_str(), device, allowRoot); });
    return mounted.value_or(false);
}

bool FileDeviceInterface::Unmount(std::string_view mountPoint) const
{
    // Deliberately allowed after a fault: leaving one of our devices mounted under a name the
    // game uses is worse than one more guarded call.
    if (m_unmount == 0)
    {
        return false;
    }

    const std::string terminated(mountPoint);
    const auto unmount = reinterpret_cast<UnmountFn>(m_unmount);
    return SafeCall("fiDevice::Unmount", [&] { unmount(terminated.c_str()); });
}

bool FileDeviceInterface::CanReadFile(std::string_view path, std::size_t byteCount) const
{
    using OpenFn = uint64_t (*)(void* self, const char* path, bool readOnly);
    using ReadFn = uint32_t (*)(void* self, uint64_t handle, void* buffer, uint32_t size);
    using CloseFn = int32_t (*)(void* self, uint64_t handle);

    void* const device = GetDevice(path, true);
    if (device == nullptr)
    {
        return false;
    }

    const std::string terminated(path);
    const auto self = reinterpret_cast<uintptr_t>(device);
    std::vector<std::byte> buffer(byteCount);
    const std::optional<bool> readable = SafeCall(
        "fiDevice::Read",
        [&]
        {
            const auto open = GetVirtualFunction<OpenFn>(self, FileDeviceLayout::kSlotOpen);
            const uint64_t handle = open(device, terminated.c_str(), true);
            if (handle == kInvalidFileHandleRaw)
            {
                return false;
            }
            const auto read = GetVirtualFunction<ReadFn>(self, FileDeviceLayout::kSlotRead);
            const uint32_t count =
                read(device, handle, buffer.data(), static_cast<uint32_t>(buffer.size()));
            GetVirtualFunction<CloseFn>(self, FileDeviceLayout::kSlotClose)(device, handle);
            return count == buffer.size();
        });
    return readable.value_or(false);
}

Result<void*> FileDeviceInterface::CreateRelativeDevice(void* storage, std::size_t storageBytes,
                                                        bool& constructed,
                                                        const std::string& root) const
{
    if (constructed)
    {
        return MakeError(ErrorCode::Unavailable, "the device has already been constructed");
    }

    void* const device = storage;
    const auto self = reinterpret_cast<uintptr_t>(device);
    const bool built = SafeCall("fiDeviceRelative::fiDeviceRelative",
                                [&]
                                {
                                    std::memset(storage, 0, storageBytes);
                                    *reinterpret_cast<uintptr_t*>(self) = m_relativeVftable;
                                    *reinterpret_cast<char*>(self + kRelativePadTerminator) = '\0';
                                });
    if (!built)
    {
        return MakeError(ErrorCode::Unavailable, "constructing the device faulted");
    }
    constructed = true;

    // The game's SetPath takes (this, path, allowRoot, baseDevice); a null base device means
    // "resolve against the real filesystem", which is what fiDeviceLocal ends up doing.
    const bool pathSet = SafeCall("fiDeviceRelative::SetPath",
                                  [&]
                                  {
                                      const auto setPath =
                                          reinterpret_cast<SetPathFn>(m_relativeSetPath);
                                      setPath(device, root.c_str(), true, nullptr);
                                  });
    if (!pathSet)
    {
        return MakeError(ErrorCode::Unavailable, "fiDeviceRelative::SetPath faulted");
    }
    return device;
}

Result<void> FileDeviceInterface::MountResourcesRoot(const std::filesystem::path& root)
{
    if (m_relativeVftable == 0 || HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "the device layer is unusable");
    }

    if (m_mountedDevice != nullptr)
    {
        if (m_resourcesRoot == root)
        {
            return {}; // a second Initialize() after a session reload
        }
        return MakeError(ErrorCode::NotSupported,
                         "'{}' is already mounted at '{}' and RAGE mounts live for the whole "
                         "process, so '{}' cannot replace it",
                         util::ToUtf8(m_resourcesRoot), kResourcesMountPoint, util::ToUtf8(root));
    }

    const std::string deviceRoot = MakeDeviceRoot(root);
    if (!IsPrintableAscii(deviceRoot))
    {
        // VERIFY on a real build: RAGE devices take UTF-8, but nothing proves its path
        // normalization survives non-ASCII.
        SPL_LOG_WARNING(Rage,
                        "The '{}' folder '{}' is not plain ASCII; RAGE path handling for "
                        "it is unverified",
                        kResourcesMountPoint, deviceRoot);
    }

    Result<void*> device = CreateRelativeDevice(g_resourcesDeviceStorage, kRelativeDeviceBytes,
                                                g_resourcesDeviceConstructed, deviceRoot);
    if (!device)
    {
        return device.GetError();
    }

    const std::string mountName{kResourcesMountPoint};
    const bool mounted = SafeCall("fiDeviceRelative::Mount",
                                  [&]
                                  {
                                      const auto mount = reinterpret_cast<MountFn>(m_relativeMount);
                                      mount(device.GetValue(), mountName.c_str(), true);
                                  });
    if (!mounted)
    {
        return MakeError(ErrorCode::Unavailable, "fiDeviceRelative::Mount faulted");
    }

    const void* const front = PutLooseResourceDeviceInFront(device.GetValue(), mountName);

    // The mount only counts once the device stack hands the same object back.
    if (void* const resolved = GetDevice(mountName, true); resolved != front)
    {
        return MakeError(ErrorCode::Unavailable,
                         "'{}' resolves to {} instead of the device we mounted", mountName,
                         fmt::ptr(resolved));
    }

    m_mountedDevice = const_cast<void*>(front);
    m_resourcesRoot = root;
    SPL_LOG_DEBUG(Rage, "Mounted '{}' at '{}'", deviceRoot, mountName);

    // Reading the root back through the mount proves the device resolves paths, not just that
    // it is in the table. It is a diagnostic rather than a failure, because no real build has
    // confirmed that GetFileAttributes answers for a directory.
    if (!GetFileAttributes(m_mountedDevice, mountName))
    {
        SPL_LOG_WARNING(Rage,
                        "'{}' is mounted but the device reports no attributes for it; files "
                        "under it may not open",
                        mountName);
    }
    return {};
}

Result<void> FileDeviceInterface::MountModsRoot(const std::filesystem::path& root,
                                                const IDeviceFileSource& mods)
{
    if (m_modsDevice != nullptr)
    {
        if (m_modsRoot == root)
        {
            return {}; // a second Initialize() after a session reload
        }
        return MakeError(ErrorCode::NotSupported,
                         "the mods are already mounted at '{}' for '{}', and RAGE mounts live "
                         "for the whole process",
                         kModsMountPoint, util::ToUtf8(m_modsRoot));
    }

    ModArchiveDevice& device =
        *m_modDevices.emplace_back(std::make_unique<ModArchiveDevice>(mods, ""));
    if (Result<void> mounted =
            MountAndVerify(kModsMountPoint, device.GetGameDevice(), util::ToUtf8Generic(root));
        !mounted)
    {
        return mounted.GetError();
    }
    m_modsDevice = device.GetGameDevice();
    m_modsRoot = root;
    return {};
}

Result<bool> FileDeviceInterface::MountOverlay(const IDeviceFileSource& mods, std::string folder,
                                               std::string_view mountPoint,
                                               std::string_view probeFile)
{
    if (m_mountGlobal == 0 || HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable,
                         "signature 'rage::fiDevice::MountGlobal' did not resolve");
    }
    ModArchiveDevice& device =
        *m_modDevices.emplace_back(std::make_unique<ModArchiveDevice>(mods, std::move(folder)));
    if (!MountGlobal(mountPoint, device.GetGameDevice(), true))
    {
        return MakeError(ErrorCode::Unavailable, "the game refused the device");
    }
    m_overlays.push_back(device.GetGameDevice());
    return IsServedByOverlay(std::string{mountPoint} + std::string{probeFile});
}

std::vector<std::size_t> FileDeviceInterface::GetUnexpectedModDeviceSlots() const
{
    std::vector<std::size_t> slots;
    for (const std::unique_ptr<ModArchiveDevice>& device : m_modDevices)
    {
        for (const std::size_t slot : device->GetUnexpectedSlots())
        {
            if (std::ranges::find(slots, slot) == slots.end())
            {
                slots.push_back(slot);
            }
        }
    }
    std::ranges::sort(slots);
    return slots;
}

void* FileDeviceInterface::PutLooseResourceDeviceInFront(void* relativeDevice,
                                                         const std::string& mountPoint)
{
    if (m_mountGlobal == 0)
    {
        SPL_LOG_WARNING(Rage,
                        "Resources larger than 16 MiB under '{}' will not load: signature "
                        "'rage::fiDevice::MountGlobal' did not resolve",
                        mountPoint);
        return relativeDevice;
    }
    LooseResourceDevice& front =
        *m_looseResourceDevices.emplace_back(std::make_unique<LooseResourceDevice>(relativeDevice));
    if (!MountGlobal(mountPoint, front.GetGameDevice(), true))
    {
        SPL_LOG_WARNING(Rage,
                        "Resources larger than 16 MiB under '{}' will not load: the game "
                        "refused the device that serves them",
                        mountPoint);
        return relativeDevice;
    }
    return front.GetGameDevice();
}

bool FileDeviceInterface::IsServedByOverlay(std::string_view path) const
{
    void* const device = GetDevice(path, true);
    return device != nullptr && std::ranges::find(m_overlays, device) != m_overlays.end();
}

Result<void> FileDeviceInterface::MountAndVerify(std::string_view mountPoint, void* device,
                                                 std::string_view rootForLog)
{
    if (m_mountGlobal == 0 || HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable,
                         "signature 'rage::fiDevice::MountGlobal' did not resolve");
    }
    if (!MountGlobal(mountPoint, device, true))
    {
        return MakeError(ErrorCode::Unavailable, "the game refused the device for '{}'",
                         mountPoint);
    }
    // The mount only counts once the device stack hands the same object back.
    if (void* const resolved = GetDevice(mountPoint, true); resolved != device)
    {
        return MakeError(ErrorCode::Unavailable,
                         "'{}' resolves to {} instead of the device we mounted", mountPoint,
                         fmt::ptr(resolved));
    }
    SPL_LOG_DEBUG(Rage, "Mounted '{}' at '{}'", rootForLog, mountPoint);
    return {};
}

std::optional<std::string>
FileDeviceInterface::ToVfsPath(const std::filesystem::path& absoluteFile) const
{
    const std::optional<std::string> resourcesPath =
        m_mountedDevice != nullptr ? MakeVfsPath(m_resourcesRoot, absoluteFile) : std::nullopt;
    const std::optional<std::string> modsPath =
        m_modsDevice != nullptr ? MakeVfsPath(m_modsRoot, kModsMountPoint, absoluteFile)
                                : std::nullopt;
    // When one root is inside the other, the file belongs to the deeper one.
    if (resourcesPath && modsPath)
    {
        const bool modsIsDeeper = m_modsRoot.native().size() > m_resourcesRoot.native().size();
        return modsIsDeeper ? modsPath : resourcesPath;
    }
    return resourcesPath ? resourcesPath : modsPath;
}

Result<void> FileDeviceInterface::Verify(const memory::Module& image) const
{
    for (std::string_view mount : kWellKnownMounts)
    {
        void* device = GetDevice(mount, true);
        if (device == nullptr)
        {
            return MakeError(ErrorCode::NotFound, "no device is mounted at '{}'", mount);
        }
        // Once a mod overlay is mounted, the device in front is our own, whose vtable lives in this
        // DLL and which fronts no game device.
        if (std::ranges::find(m_overlays, device) != m_overlays.end())
        {
            continue;
        }
        if (const auto own = std::ranges::find(m_looseResourceDevices, device,
                                               [](const std::unique_ptr<LooseResourceDevice>& d)
                                               { return d->GetGameDevice(); });
            own != m_looseResourceDevices.end())
        {
            device = (*own)->GetRealDevice();
        }
        const uintptr_t vtable = GetVtableAddress(reinterpret_cast<uintptr_t>(device));
        if (!image.Contains(vtable))
        {
            // Another ASI may mount its own device here (RageOpenV does, at both mount points).
            const std::optional<memory::Module> owner = memory::Module::FindContaining(vtable);
            if (!owner)
            {
                return MakeError(
                    ErrorCode::NotFound,
                    "the device at '{}' has vtable {:#x}, which is in no loaded module", mount,
                    vtable);
            }
            SPL_LOG_INFO(Rage, "Device at '{}' was mounted by another module ({}+{:#x})", mount,
                         owner->GetFileName(), vtable - owner->GetBase());
            continue;
        }
        // GetName's slot is the one ordinal we are least sure of, so it is logged, not trusted.
        SPL_LOG_DEBUG(Rage, "Device at '{}' is {}+{:#x}, reported name '{}'", mount,
                      image.GetFileName(), vtable - image.GetBase(), GetDeviceName(device));
    }

    // Our own device gets the game's vtable verbatim, so a wrong address here would be a
    // wild jump on the first read through the mount.
    if (!image.Contains(m_relativeVftable))
    {
        return MakeError(ErrorCode::NotFound, "the fiDeviceRelative vtable at {:#x} is outside {}",
                         m_relativeVftable, image.GetFileName());
    }
    return {};
}
} // namespace spl::rage
