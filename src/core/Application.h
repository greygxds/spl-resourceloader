#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/LoaderConfig.h"
#include "console/CommandRegistry.h"
#include "console/ConsoleInput.h"
#include "core/CrashReport.h"
#include "core/Paths.h"
#include "core/SessionGuard.h"
#include "mods/ModCatalog.h"
#include "mods/ModLayout.h"
#include "rage/RageBridge.h"
#include "rage/RageStreamingBackend.h"
#include "resource/ResourceManager.h"
#include "streaming/StreamingManager.h"
#include "streaming/StreamingPlan.h"

// Forward-declared so this header stays free of <Windows.h> (conventions section 7).
struct HINSTANCE__;
using SplModuleHandle = HINSTANCE__*;

namespace spl
{
/// Where the loader ended up. Degraded is the interesting one: everything that does not need
/// the game ran and was logged, and nothing was registered.
enum class LoaderState
{
    Uninitialized,
    Disabled, ///< loader.enabled = false, or the loader could not set itself up
    Degraded, ///< the RAGE bridge is unusable on this build
    SafeMode, ///< the previous session crashed and nobody knows which resource did it
    Ready
};

[[nodiscard]] std::string_view ToString(LoaderState state);

/// What an attempt to start with the game came to.
enum class EarlyStart
{
    NotReady, ///< the game's code is still encrypted: ask again on a later call
    Started,  ///< the startup hooks are in; ScriptMain only has to keep ticking
    Declined  ///< disabled, safe mode, or a hook missing: the loader starts with story mode
};

/// Process-wide orchestrator. ScriptHookV may call ScriptMain again after a session reload,
/// so Initialize() is idempotent and Application is a singleton.
class Application
{
public:
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    [[nodiscard]] static Application& Instance();

    /// Records the ASI module handle. Called from DllMain, so it does no work.
    static void SetModuleHandle(SplModuleHandle module);

    /// Resolves paths, loads the configuration, starts logging and reports what it found.
    /// Returns false when the loader must not run, either because it could not set itself up
    /// or because the user disabled it; the ASI then stays loaded and does nothing.
    [[nodiscard]] bool Initialize();

    /// Called by the start gate on the game's main thread, as GTA5.exe starts, until it answers
    /// something other than NotReady. Sets up everything that needs no game state, and hooks the
    /// game's startup so resources load as they do in FiveM.
    [[nodiscard]] EarlyStart TryStartEarly();

    /// One pass of the runtime loop: runs console commands, then pumps streaming registration.
    void Tick();

    /// Best-effort teardown. Makes no game calls, because it runs from DllMain.
    void Shutdown();

    /// Resolved paths; std::nullopt until Initialize() has run.
    [[nodiscard]] const std::optional<Paths>& GetPaths() const
    {
        return m_paths;
    }

    /// The effective configuration, defaults included.
    [[nodiscard]] const config::LoaderConfig& GetConfig() const
    {
        return m_config;
    }

    /// Every discovered resource, in load order.
    [[nodiscard]] resource::ResourceManager& GetResources()
    {
        return m_resources;
    }

    /// What the loader intends to register, built during Initialize(). Empty when streaming
    /// is disabled.
    [[nodiscard]] const streaming::StreamingPlan& GetStreamingPlan() const
    {
        return m_plan;
    }

    /// The bridge into game memory. Only usable once IsReady() says so.
    [[nodiscard]] rage::RageBridge& GetBridge()
    {
        return m_bridge;
    }

    [[nodiscard]] const streaming::StreamingManager& GetStreaming() const
    {
        return m_streaming;
    }

    [[nodiscard]] LoaderState GetState() const
    {
        return m_state;
    }

private:
    Application() = default;

    /// Paths, configuration, logging, crash safety, discovery and the plan: everything that needs
    /// no game state. Runs once; later calls return the first answer.
    [[nodiscard]] bool Bootstrap();

    /// Brings the RAGE bridge up and starts streaming, or drops to Degraded when the bridge
    /// cannot be trusted. insideGameStartup lifts the per-tick limits.
    void ConnectToGame(bool insideGameStartup);

    /// The game has mounted its own devices and read nothing yet.
    void OnGameMounted();
    void OnInitPhaseStart(rage::InitPhase phase);
    void OnInitPhaseEnd(rage::InitPhase phase);

    /// Ticks the pump until it reaches stop or finishes, inside the game's startup.
    void PumpStreaming(streaming::StreamingStage stop);

    /// The enabled resources' level metas as VFS paths. Needs the resources root mounted.
    [[nodiscard]] rage::LevelMetas CollectLevelMetas() const;

    /// One warning per resource whose level metas cannot load this session.
    void WarnAboutLevelMetas() const;

    /// Applies streaming.mp_maps. Only before the game sets up its map layer; a failure keeps
    /// story mode's maps and starting early goes ahead.
    void EnableMultiplayerMaps();

    /// Applies the [memory] extensions the config asks for. Only while the game starts.
    void ExtendMemoryBudgets();

    /// One warning when [memory] asks for an extension that cannot apply this session.
    void WarnAboutMemoryBudgets() const;

    /// Mounts the resources folder, the mods and the mods' overlays, once.
    void MountGameRoots();

    /// Mounts every adopted mod's common/ and platform/ over the game's, in mod order.
    void MountModOverlays();

    /// Warns about every overlay whose probe file the game serves from its own devices again.
    void CheckModOverlays() const;

    /// Reports what the previous session left behind and returns whether to run in safe mode.
    [[nodiscard]] bool ApplySessionStartup(const SessionStartup& startup);

    /// Scans mods/, lays each out from its archive and adopts them as resources sorted after
    /// everything discovered, so same-named files lose to resources.
    void DiscoverMods(std::span<const std::string> quarantined);

    /// Called from the crash handler: what only the application knows.
    [[nodiscard]] CrashReportInfo DescribeForCrash() const;

    /// A game call faulted while a resource was registering: that resource is quarantined for
    /// the next session. Runs once.
    void QuarantineFaultedResource();

    /// Once registration has finished: one info line per resource and per mod with their totals,
    /// the ready line, and the stage timings at debug.
    void LogLoadSummary();

    /// The data files of mods that replace a file the game itself reads from common:/ or
    /// platform:/, as absolute paths. Asked before the overlays are mounted, while the game's
    /// own devices still answer.
    [[nodiscard]] std::vector<std::string> FindMetasTheGameReads() const;

    /// Runs the lines typed into the console since the last tick. Script thread only.
    void RunConsoleCommands();

    std::optional<Paths> m_paths;
    config::LoaderConfig m_config;
    resource::ResourceManager m_resources;
    std::filesystem::path m_resourcesRoot;
    std::filesystem::path m_modsRoot; ///< mod resources' root; empty when none was adopted

    /// The adopted mods' files, which the game reads through the devices mounted over it. Kept
    /// for the whole process: the game keeps the devices.
    mods::ModCatalog m_modCatalog;

    /// The adopted mods' common/ and platform/ folders, mounted over the game's once the bridge
    /// is up, in mod order.
    std::vector<mods::ModLayout::OverlayRoot> m_modOverlays;
    streaming::StreamingPlan m_plan;
    rage::RageBridge m_bridge;
    std::unique_ptr<rage::RageStreamingBackend> m_backend;
    streaming::StreamingManager m_streaming;
    console::CommandRegistry m_commands;
    console::ConsoleInput m_consoleInput;
    SessionGuard m_session;
    int64_t m_discoveryMs = 0;
    int64_t m_bridgeMs = 0;
    bool m_faultHandled = false;
    bool m_summaryLogged = false;
    LoaderState m_state = LoaderState::Uninitialized;
    bool m_initialized = false;

    enum class BootstrapResult
    {
        NotRun,
        Succeeded,
        Failed
    };
    BootstrapResult m_bootstrap = BootstrapResult::NotRun;
    bool m_startedEarly = false;
    bool m_connected = false; ///< ConnectToGame ran, whatever it came to
    bool m_rootsMounted = false;
    bool m_sessionRegistrationDone = false;

    /// A pump that has not finished after this many unlimited ticks is stuck.
    static constexpr std::size_t kMaxStartupTicks = 10000;

    /// Registrations the game replaced are put back this often, about once a second.
    static constexpr std::size_t kReassertIntervalTicks = 60;
    std::size_t m_ticksSinceReassert = 0;
};
} // namespace spl
