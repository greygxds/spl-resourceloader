#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Result.h"
#include "memory/Module.h"
#include "rage/FileDeviceInterface.h"
#include "rage/ForcedDevice.h"
#include "rage/GameAddresses.h"
#include "rage/types/DataFileTypes.h"

namespace spl::rage
{
/// One CImapDependencies or CItypDependencies row, copied out of the chunk.
struct ManifestDependencyRow
{
    uint32_t name = 0;
    std::vector<uint32_t> dependencies;
};

/// What a .ymf declared, read back from the game's chunk before it is committed.
struct ParsedPackfileManifest
{
    std::vector<ManifestDependencyRow> mapData;  ///< a .ymap and the .ytyp files it needs
    std::vector<ManifestDependencyRow> mapTypes; ///< a .ytyp and what it needs
};

/// Feeds a resource's .ymf to the game's packfile manifest loader, the way a DLC pack's
/// _manifest.ymf is fed. Parse and Commit are separate because a .ytyp the
/// manifest takes over has to be released from its DLC_ITYP_REQUEST in between.
class ManifestChunkLoader
{
public:
    void Initialize(const GameAddresses& addresses);

    /// Every address this needs resolved, and the chunk sits in the image. An error names the
    /// first thing missing; it disables .ymf loading, nothing else.
    [[nodiscard]] Result<void> Verify(const memory::Module& image,
                                      const FileDeviceInterface& files) const;

    /// Mounts a forced device at "localPack:/" for vfsPath, lets the game parse it, unmounts,
    /// and copies the dependencies out. On success Commit() must follow; on failure the chunk
    /// has already been cleared.
    [[nodiscard]] Result<ParsedPackfileManifest>
    Parse(const FileDeviceInterface& files, std::string_view vfsPath, std::string_view tagName);

    /// Hands the parsed chunk to the map store and clears it.
    [[nodiscard]] Result<void> Commit();

    /// True when RPF_FILE entries can be mounted: Verify passed and AddPackfile resolved.
    [[nodiscard]] bool CanMountPackfiles() const
    {
        return m_addPackfile != 0;
    }

    /// Mounts an RPF_FILE the way FiveM's packfile mounter does: init the chunk, add the
    /// packfile, load and clear the chunk (LoadStreamingFile.cpp:962). The entry must outlive
    /// the process, which DataFileInterface::CreateEntry guarantees.
    [[nodiscard]] Result<void> MountPackfile(DataFileEntryView* entry);

private:
    /// Copies both dependency arrays out of the chunk. std::nullopt when they do not look like
    /// arrays or reading them faulted.
    [[nodiscard]] std::optional<ParsedPackfileManifest> ReadDependencies() const;

    [[nodiscard]] bool CallChunkFunction(const char* what, uintptr_t function) const;

    uintptr_t m_chunk = 0;
    uintptr_t m_loadPackfileManifest = 0;
    uintptr_t m_initChunk = 0;
    uintptr_t m_loadChunk = 0;
    uintptr_t m_clearChunk = 0;
    uintptr_t m_addPackfile = 0; ///< optional: RPF_FILE only

    /// Kept for the process, like FiveM's: nothing proves Unmount drops every reference.
    std::vector<std::unique_ptr<ForcedDevice>> m_devices;
};
} // namespace spl::rage
