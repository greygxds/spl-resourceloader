#include "rage/RageBridge.h"

#include <optional>

#include <main.h> // ScriptHookV SDK, for getGameVersion()

#include "logging/Logger.h"
#include "memory/Module.h"
#include "rage/SafeCall.h"

namespace spl::rage
{
std::string_view ToString(BridgeState state)
{
    using enum BridgeState;
    switch (state)
    {
    case Uninitialized:
        return "uninitialized";
    case Unsupported:
        return "unsupported game";
    case Unresolved:
        return "signatures unresolved";
    case Unverified:
        return "verification failed";
    case Ready:
        return "ready";
    case Faulted:
        return "faulted";
    }
    return "uninitialized";
}

Result<void> RageBridge::Resolve(const config::LoaderConfig& config)
{
    if (m_resolved)
    {
        return {};
    }
    const std::optional<GameBuild> detected = DetectGameBuild();
    if (!detected)
    {
        m_state = BridgeState::Unsupported;
        return MakeError(ErrorCode::NotSupported,
                         "the game build could not be read, so no game memory will be touched");
    }

    m_build = *detected;
    m_verification = ClassifyBuild(m_build.build);
    if (!m_build.IsSupported())
    {
        SPL_LOG_INFO(Rage, "Game build {}", m_build.ToString());
        m_state = BridgeState::Unsupported;
        return MakeError(ErrorCode::NotSupported, "the {} edition is not supported",
                         ToString(m_build.edition));
    }
    // Decided before anything is hooked, which early init does long before the layout checks.
    if (m_verification == BuildVerification::NewerUnverified &&
        !config.loader.allowUnverifiedBuilds)
    {
        SPL_LOG_INFO(Rage, "Game build {} ({})", m_build.ToString(), ToString(m_verification));
        m_state = BridgeState::Unsupported;
        return MakeError(ErrorCode::NotSupported,
                         "build {} is newer than {}, the newest verified build, and "
                         "loader.allow_unverified_builds is false",
                         m_build.build, GetNewestVerifiedBuild());
    }

    const memory::Module image = memory::Module::Main();
    SPL_LOG_DEBUG(Rage, "{} at {:#x}, {} bytes, {} scan region(s)", image.GetFileName(),
                  image.GetBase(), image.GetSizeBytes(), image.GetScanRegions().size());

    m_signatures = AddressResolver::ResolveAll(image, m_build);
    SPL_LOG_INFO(Rage, "Game build {} ({}): {}/{} signatures found in {} ms{}", m_build.ToString(),
                 ToString(m_verification), m_signatures.addresses.size(), m_signatures.applicable,
                 m_signatures.durationMs,
                 m_signatures.optionalMissing > 0
                     ? fmt::format(" ({} optional missing, so some features are off)",
                                   m_signatures.optionalMissing)
                     : std::string{});
    Result<GameAddresses> addresses = GameAddresses::Build(m_signatures);
    if (!addresses)
    {
        m_state = BridgeState::Unresolved;
        return addresses.GetError();
    }
    m_addresses = addresses.GetValue();
    m_resolved = true;
    return {};
}

Result<void> RageBridge::PrepareFileDevices()
{
    if (!m_resolved)
    {
        return MakeError(ErrorCode::Unavailable, "the signatures are not resolved");
    }
    if (Result<void> files = m_files.Initialize(m_addresses); !files)
    {
        return files;
    }
    return m_files.Verify(memory::Module::Main());
}

Result<void> RageBridge::Initialize(const config::LoaderConfig& config)
{
    if (Result<void> resolved = Resolve(config); !resolved)
    {
        return resolved;
    }

    // ScriptHookV maps the build to its own eGameVersion enum and lags behind new builds, so
    // it is logged as a cross-check only: a mismatch is interesting, never authoritative.
    SPL_LOG_DEBUG(Rage, "ScriptHookV reports game version {}",
                  static_cast<int>(::getGameVersion()));
    const memory::Module image = memory::Module::Main();

    if (Result<void> streaming = m_streaming.Initialize(m_addresses, m_build); !streaming)
    {
        m_state = BridgeState::Unresolved;
        return streaming.GetError();
    }
    if (Result<void> files = m_files.Initialize(m_addresses); !files)
    {
        m_state = BridgeState::Unresolved;
        return files.GetError();
    }
    if (Result<void> dataFiles = m_dataFiles.Initialize(m_addresses); !dataFiles)
    {
        m_state = BridgeState::Unresolved;
        return dataFiles.GetError();
    }
    m_patches.Initialize(m_addresses, m_build);
    m_manifests.Initialize(m_addresses);
    m_mapStore.Initialize(m_addresses, m_build);
    m_rawStreamer.Initialize(m_addresses);
    m_interiors.Initialize(m_addresses);
    m_overrideHooks.Initialize(m_addresses);
    m_content.Initialize(m_addresses);

    // Everything from here on reads game memory, so a wrong address shows up as a failed
    // check or a caught fault rather than as a crash.
    if (Result<void> verified = m_streaming.Verify(image); !verified)
    {
        m_state = BridgeState::Unverified;
        return verified.GetError();
    }
    if (Result<void> verified = m_files.Verify(image); !verified)
    {
        m_state = BridgeState::Unverified;
        return verified.GetError();
    }
    if (Result<void> verified = m_dataFiles.Verify(image); !verified)
    {
        m_state = BridgeState::Unverified;
        return verified.GetError();
    }
    if (HasFaulted())
    {
        m_state = BridgeState::Faulted;
        return MakeError(ErrorCode::Unavailable, "a game call faulted during verification");
    }

    // A game update nobody has checked yet. Everything above passed, which is as much as the
    // loader can prove on its own; the map-store replay still checks its patch sites by itself.
    if (m_verification == BuildVerification::NewerUnverified)
    {
        SPL_LOG_WARNING(Rage,
                        "Build {} is not verified (newest verified: {}); proceeding because all "
                        "signatures resolved and every layout check passed",
                        m_build.build, GetNewestVerifiedBuild());
    }

    // Maps are the newest and most build-sensitive support, so a miss there costs only maps. The
    // reason is reported when a resource actually needs them, not to every textures-only user.
    m_manifestSupport = m_manifests.Verify(image, m_files);
    m_mapReloadSupport = m_mapStore.Verify(image, m_streaming);
    m_overrideSupport = m_rawStreamer.Verify(image);
    if (!m_manifestSupport)
    {
        SPL_LOG_DEBUG(Rage, "Packfile manifest loading unavailable: {}",
                      m_manifestSupport.GetMessage());
    }
    if (!m_mapReloadSupport)
    {
        SPL_LOG_DEBUG(Rage, "Map store reload unavailable: {}", m_mapReloadSupport.GetMessage());
    }
    if (!m_overrideSupport)
    {
        SPL_LOG_DEBUG(Rage, "Game asset overrides unavailable: {}", m_overrideSupport.GetMessage());
    }

    if (config.diagnostics.dumpStreamingModules)
    {
        m_streaming.DumpModules(image);
        m_dataFiles.DumpMounters(image);
    }

    m_state = BridgeState::Ready;
    SPL_LOG_INFO(Rage, "RAGE bridge ready");
    return {};
}

BridgeState RageBridge::GetState() const
{
    if (m_state == BridgeState::Ready && HasFaulted())
    {
        return BridgeState::Faulted;
    }
    return m_state;
}

bool RageBridge::IsReady() const
{
    return GetState() == BridgeState::Ready;
}
} // namespace spl::rage
