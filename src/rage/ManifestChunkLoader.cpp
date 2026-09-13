#include "rage/ManifestChunkLoader.h"

#include <optional>
#include <span>
#include <string>
#include <utility>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/SafeCall.h"
#include "rage/types/MapStoreTypes.h"

namespace spl::rage
{
namespace
{
using ChunkFn = void (*)(void* chunk);
using LoadPackfileManifestFn = void (*)(void* chunk, void* packfile, const char* tagName);

/// Copies one atArray of dependency rows. False when the array does not look like one.
[[nodiscard]] bool CopyRows(const atArrayView<ManifestDependenciesView>& array,
                            std::vector<ManifestDependencyRow>& rows)
{
    if (array.count > ManifestChunkLayout::kMaxDependencyEntries ||
        (array.count > 0 && array.data == nullptr))
    {
        return false;
    }
    for (const ManifestDependenciesView& view : std::span(array.data, array.count))
    {
        if (view.dependencies.count > ManifestChunkLayout::kMaxDependencyEntries ||
            (view.dependencies.count > 0 && view.dependencies.data == nullptr))
        {
            return false;
        }
        const std::span<const uint32_t> hashes(view.dependencies.data, view.dependencies.count);
        rows.push_back(ManifestDependencyRow{.name = view.name,
                                             .dependencies = {hashes.begin(), hashes.end()}});
    }
    return true;
}
} // namespace

void ManifestChunkLoader::Initialize(const GameAddresses& addresses)
{
    m_chunk = addresses.manifestChunk;
    m_loadPackfileManifest = addresses.loadPackfileManifest;
    m_initChunk = addresses.initManifestChunk;
    m_loadChunk = addresses.loadManifestChunk;
    m_clearChunk = addresses.clearManifestChunk;
    m_addPackfile = addresses.dataFileMgrAddPackfile;
}

Result<void> ManifestChunkLoader::Verify(const memory::Module& image,
                                         const FileDeviceInterface& files) const
{
    const std::pair<std::string_view, uintptr_t> required[] = {
        {"ManifestChunk", m_chunk},           {"LoadPackfileManifest", m_loadPackfileManifest},
        {"InitManifestChunk", m_initChunk},   {"LoadManifestChunk", m_loadChunk},
        {"ClearManifestChunk", m_clearChunk},
    };
    for (const auto& [name, address] : required)
    {
        if (!image.Contains(address))
        {
            return MakeError(ErrorCode::NotFound, "signature '{}' did not resolve", name);
        }
    }
    if (!files.CanMountGlobally())
    {
        return MakeError(ErrorCode::NotFound,
                         "signature 'rage::fiDevice::MountGlobal' or 'rage::fiDevice::Unmount' "
                         "did not resolve");
    }
    return {};
}

bool ManifestChunkLoader::CallChunkFunction(const char* what, uintptr_t function) const
{
    const auto call = reinterpret_cast<ChunkFn>(function);
    return SafeCall(what, [&] { call(reinterpret_cast<void*>(m_chunk)); });
}

Result<ParsedPackfileManifest> ManifestChunkLoader::Parse(const FileDeviceInterface& files,
                                                          std::string_view vfsPath,
                                                          std::string_view tagName)
{
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }

    // The game's parser does not survive a file it cannot read, so prove it reads first.
    if (!files.CanReadFile(vfsPath, ManifestChunkLayout::kProbeBytes))
    {
        return MakeError(ErrorCode::NotFound, "'{}' cannot be read through the device stack",
                         vfsPath);
    }
    void* const realDevice = files.GetDevice(vfsPath, true);
    if (realDevice == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "no device serves '{}'", vfsPath);
    }

    ForcedDevice& device =
        *m_devices.emplace_back(std::make_unique<ForcedDevice>(realDevice, std::string{vfsPath}));

    if (!CallChunkFunction("InitManifestChunk", m_initChunk))
    {
        return MakeError(ErrorCode::Unavailable, "InitManifestChunk faulted");
    }

    const std::string mountPoint{ManifestChunkLayout::kMountPoint};
    if (!files.MountGlobal(mountPoint, device.GetGameDevice(), true))
    {
        (void)CallChunkFunction("ClearManifestChunk", m_clearChunk);
        return MakeError(ErrorCode::Unavailable, "the game refused to mount '{}'", mountPoint);
    }

    const std::string tag{tagName};
    const auto loadManifest = reinterpret_cast<LoadPackfileManifestFn>(m_loadPackfileManifest);
    const bool parsed =
        SafeCall("LoadPackfileManifest",
                 [&]
                 {
                     loadManifest(reinterpret_cast<void*>(m_chunk),
                                  reinterpret_cast<void*>(ManifestChunkLayout::kPackfileArgument),
                                  tag.c_str());
                 });

    // Unmounted on every path: the mount point is the game's, and it must not keep our device.
    if (!files.Unmount(mountPoint))
    {
        SPL_LOG_ERROR(Rage, "'{}' could not be unmounted after parsing '{}'", mountPoint, vfsPath);
    }
    if (!parsed)
    {
        return MakeError(ErrorCode::Unavailable, "the game's manifest parser faulted on '{}'",
                         vfsPath);
    }

    std::optional<ParsedPackfileManifest> dependencies = ReadDependencies();
    if (!dependencies)
    {
        (void)CallChunkFunction("ClearManifestChunk", m_clearChunk);
        return MakeError(ErrorCode::NotFound,
                         "the manifest chunk does not hold dependency arrays where expected");
    }
    return std::move(*dependencies);
}

std::optional<ParsedPackfileManifest> ManifestChunkLoader::ReadDependencies() const
{
    const auto* const chunk = reinterpret_cast<const ManifestChunkView*>(m_chunk);
    std::optional<ParsedPackfileManifest> result = ParsedPackfileManifest{};
    const std::optional<bool> copied =
        SafeCall("ManifestChunk::Dependencies",
                 [&]
                 {
                     return CopyRows(chunk->mapDataDependencies, result->mapData) &&
                            CopyRows(chunk->mapTypesDependencies, result->mapTypes);
                 });
    if (!copied || !*copied)
    {
        return std::nullopt;
    }
    return result;
}

Result<void> ManifestChunkLoader::Commit()
{
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }
    if (!CallChunkFunction("LoadManifestChunk", m_loadChunk))
    {
        return MakeError(ErrorCode::Unavailable, "LoadManifestChunk faulted");
    }
    if (!CallChunkFunction("ClearManifestChunk", m_clearChunk))
    {
        return MakeError(ErrorCode::Unavailable, "ClearManifestChunk faulted");
    }
    return {};
}
Result<void> ManifestChunkLoader::MountPackfile(DataFileEntryView* entry)
{
    using AddPackfileFn = void (*)(DataFileEntryView* entry);

    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }
    if (m_addPackfile == 0)
    {
        return MakeError(ErrorCode::NotSupported,
                         "signature 'CDataFileMgr::AddPackfile' did not resolve");
    }

    entry->disabled = true; // FiveM sets it before mounting; the game's own mounter skips it
    if (!CallChunkFunction("InitManifestChunk", m_initChunk))
    {
        return MakeError(ErrorCode::Unavailable, "InitManifestChunk faulted");
    }
    const auto addPackfile = reinterpret_cast<AddPackfileFn>(m_addPackfile);
    if (!SafeCall("CDataFileMgr::AddPackfile", [&] { addPackfile(entry); }))
    {
        return MakeError(ErrorCode::Unavailable, "the game faulted adding packfile '{}'",
                         entry->name);
    }
    return Commit();
}
} // namespace spl::rage
