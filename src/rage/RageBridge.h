#pragma once

#include <string_view>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "rage/AddressResolver.h"
#include "rage/ContentInterface.h"
#include "rage/DataFileInterface.h"
#include "rage/FileDeviceInterface.h"
#include "rage/GameAddresses.h"
#include "rage/GameBuild.h"
#include "rage/GamePatches.h"
#include "rage/InitHooks.h"
#include "rage/InteriorProxyPool.h"
#include "rage/ManifestChunkLoader.h"
#include "rage/MapStoreReloader.h"
#include "rage/MemoryBudget.h"
#include "rage/OverrideHooks.h"
#include "rage/RawStreamerInterface.h"
#include "rage/StreamingInterface.h"

namespace spl::rage
{
/// How far the bridge got. Anything but Ready means no game call is made.
enum class BridgeState
{
    Uninitialized,
    Unsupported, ///< the build is unknown or not Legacy
    Unresolved,  ///< a required signature did not match
    Unverified,  ///< the addresses resolved but the layout checks failed
    Ready,
    Faulted ///< a game call raised an access violation; the session is over for us
};

[[nodiscard]] std::string_view ToString(BridgeState state);

/// The single door between the loader and the game. It detects the build, resolves the
/// signature table, proves the layouts, and then hands out the streaming, device and data-file
/// interfaces, and the code patches.
class RageBridge
{
public:
    /// Detect, resolve, verify. The error it returns is the one to log before going Degraded;
    /// it never throws and never leaves the bridge half-usable. Reuses an earlier Resolve().
    [[nodiscard]] Result<void> Initialize(const config::LoaderConfig& config);

    /// Detects the build and resolves the signature table, reading the image and nothing else,
    /// so it is safe while the game is still starting. Idempotent.
    [[nodiscard]] Result<void> Resolve();

    /// Only meaningful after a successful Resolve().
    [[nodiscard]] const GameAddresses& GetAddresses() const
    {
        return m_addresses;
    }

    /// Brings up the device layer on its own, once the game's first mounts exist, so resources
    /// and mods can be mounted before the game reads anything.
    [[nodiscard]] Result<void> PrepareFileDevices();

    /// The hooks into the game's startup. Installed by Application, when it starts early.
    [[nodiscard]] InitHooks& Init()
    {
        return m_initHooks;
    }

    /// True only when every check passed and no game call has faulted since.
    [[nodiscard]] bool IsReady() const;

    [[nodiscard]] BridgeState GetState() const;

    [[nodiscard]] StreamingInterface& Streaming()
    {
        return m_streaming;
    }

    [[nodiscard]] const StreamingInterface& Streaming() const
    {
        return m_streaming;
    }

    [[nodiscard]] FileDeviceInterface& Files()
    {
        return m_files;
    }

    [[nodiscard]] const FileDeviceInterface& Files() const
    {
        return m_files;
    }

    [[nodiscard]] DataFileInterface& DataFiles()
    {
        return m_dataFiles;
    }

    /// Scaleform slots, paint ramps and ped stream folders; each call reports a missing address.
    [[nodiscard]] ContentInterface& Content()
    {
        return m_content;
    }

    [[nodiscard]] GamePatches& Patches()
    {
        return m_patches;
    }

    /// FiveM's memory extensions. Usable after Resolve(), which is what lets them go in while the
    /// game starts.
    [[nodiscard]] MemoryBudget& Memory()
    {
        return m_memoryBudget;
    }

    /// Only usable when GetOverrideSupport() succeeded.
    [[nodiscard]] RawStreamerInterface& RawStreamer()
    {
        return m_rawStreamer;
    }

    [[nodiscard]] OverrideHooks& Overrides()
    {
        return m_overrideHooks;
    }

    /// Read-only; empty answers when the pool signature did not resolve.
    [[nodiscard]] const InteriorProxyPool& Interiors() const
    {
        return m_interiors;
    }

    /// Only usable when GetManifestSupport() succeeded.
    [[nodiscard]] ManifestChunkLoader& Manifests()
    {
        return m_manifests;
    }

    /// Only usable when GetMapReloadSupport() succeeded.
    [[nodiscard]] MapStoreReloader& MapStore()
    {
        return m_mapStore;
    }

    /// Map support is optional per build: a missing signature there disables it, not the bridge.
    /// The error says what is missing.
    [[nodiscard]] const Result<void>& GetManifestSupport() const
    {
        return m_manifestSupport;
    }

    [[nodiscard]] const Result<void>& GetMapReloadSupport() const
    {
        return m_mapReloadSupport;
    }

    /// Whether a game asset can be overridden at all on this build. The .ytyp and
    /// map-data extras have their own checks in OverrideHooks.
    [[nodiscard]] const Result<void>& GetOverrideSupport() const
    {
        return m_overrideSupport;
    }

    /// The detected build. Only meaningful once the state is past Unsupported.
    [[nodiscard]] const GameBuild& GetBuild() const
    {
        return m_build;
    }

    [[nodiscard]] BuildVerification GetBuildVerification() const
    {
        return m_verification;
    }

    /// What the signature table resolved to on this build.
    [[nodiscard]] const ResolvedSignatures& GetSignatures() const
    {
        return m_signatures;
    }

private:
    GameBuild m_build;
    BuildVerification m_verification = BuildVerification::OlderUnverified;
    ResolvedSignatures m_signatures;
    GameAddresses m_addresses;
    StreamingInterface m_streaming;
    FileDeviceInterface m_files;
    DataFileInterface m_dataFiles;
    ContentInterface m_content;
    GamePatches m_patches;
    MemoryBudget m_memoryBudget;
    ManifestChunkLoader m_manifests;
    MapStoreReloader m_mapStore;
    RawStreamerInterface m_rawStreamer;
    OverrideHooks m_overrideHooks;
    InteriorProxyPool m_interiors;
    InitHooks m_initHooks;
    bool m_resolved = false;
    Result<void> m_manifestSupport = MakeError(ErrorCode::Unavailable, "the bridge is not ready");
    Result<void> m_mapReloadSupport = MakeError(ErrorCode::Unavailable, "the bridge is not ready");
    Result<void> m_overrideSupport = MakeError(ErrorCode::Unavailable, "the bridge is not ready");
    BridgeState m_state = BridgeState::Uninitialized;
};
} // namespace spl::rage
