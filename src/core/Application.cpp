#include "core/Application.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "config/ConfigLoader.h"
#include "config/LoaderConfig.h"
#include "core/ConsoleCommands.h"
#include "core/CrashHandler.h"
#include "core/CrashReport.h"
#include "core/LoadSummary.h"
#include "core/Paths.h"
#include "core/Result.h"
#include "core/SessionGuard.h"
#include "core/Version.h"
#include "hooking/HookManager.h"
#include "logging/Logger.h"
#include "mods/ModCatalog.h"
#include "mods/ModLayout.h"
#include "mods/ModsScanner.h"
#include "rage/GameBuild.h"
#include "rage/RageBridge.h"
#include "rage/RageStreamingBackend.h"
#include "rage/SafeCall.h"
#include "resource/ResourceManager.h"
#include "streaming/StreamingManager.h"
#include "streaming/StreamingPlan.h"
#include "util/Strings.h"

namespace spl
{
namespace
{
SplModuleHandle g_moduleHandle = nullptr;

/// Diagnostics are collected before the logger exists, so they are replayed here, once it is
/// configured. Errors first: a parse failure explains every default that follows.
void ReplayDiagnostics(const config::ConfigDiagnostics& diagnostics)
{
    for (const std::string& error : diagnostics.errors)
    {
        SPL_LOG_ERROR(Config, error);
    }
    for (const std::string& warning : diagnostics.warnings)
    {
        SPL_LOG_WARNING(Config, warning);
    }
}

/// Loaders before this one copied every mod into <data>/mods_cache. Mods are read from their
/// archives now, so the copy is only disk space: removed once, and reported.
void RemoveOldModsCache(const std::filesystem::path& dataDir)
{
    const std::filesystem::path cache = dataDir / "mods_cache";
    std::error_code error;
    if (!std::filesystem::is_directory(cache, error))
    {
        return;
    }
    uint64_t sizeBytes = 0;
    for (std::filesystem::recursive_directory_iterator entry{cache, error}, end;
         entry != end && !error; entry.increment(error))
    {
        std::error_code sizeError;
        if (entry->is_regular_file(sizeError))
        {
            sizeBytes += entry->file_size(sizeError);
        }
    }
    error.clear();
    std::filesystem::remove_all(cache, error);
    if (error)
    {
        SPL_LOG_WARNING(Mods,
                        "The old mods cache '{}' could not be removed ({}); it is no longer "
                        "used and can be deleted",
                        util::ToUtf8Generic(cache), error.message());
        return;
    }
    SPL_LOG_INFO(Mods, "Removed the old mods cache ({} MiB): mods are read from their archives now",
                 sizeBytes / (1024 * 1024));
}

int64_t MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

/// "early registration 130.2 ms (1 tick)"
std::string DescribeTiming(const streaming::StageTiming& timing)
{
    return fmt::format("{} {:.1f} ms ({} tick{}, slowest {:.1f} ms)", ToString(timing.stage),
                       static_cast<double>(timing.activeMicros) / 1000.0, timing.ticks,
                       timing.ticks == 1 ? "" : "s",
                       static_cast<double>(timing.slowestTickMicros) / 1000.0);
}
} // namespace

std::string_view ToString(LoaderState state)
{
    using enum LoaderState;
    switch (state)
    {
    case Uninitialized:
        return "uninitialized";
    case Disabled:
        return "disabled";
    case Degraded:
        return "degraded";
    case SafeMode:
        return "safe mode";
    case Ready:
        return "ready";
    }
    return "uninitialized";
}

Application& Application::Instance()
{
    static Application instance;
    return instance;
}

void Application::SetModuleHandle(SplModuleHandle module)
{
    g_moduleHandle = module;
}

bool Application::Initialize()
{
    if (m_initialized)
    {
        return true; // a second ScriptMain entry after a session reload
    }
    if (!Bootstrap())
    {
        return false;
    }
    CrashHandler::EnsureFirst();

    if (m_state != LoaderState::SafeMode && !m_connected)
    {
        if (m_startedEarly)
        {
            SPL_LOG_WARNING(Core, "The loader started with the game, but never saw its session "
                                  "start; registering from story mode instead");
        }
        WarnAboutLevelMetas();
        ConnectToGame(false);
    }

    m_initialized = true;
    return true;
}

bool Application::Bootstrap()
{
    if (m_bootstrap != BootstrapResult::NotRun)
    {
        return m_bootstrap == BootstrapResult::Succeeded;
    }
    m_bootstrap = BootstrapResult::Failed;

    m_state = LoaderState::Disabled;
    m_paths = Paths::Resolve();
    if (!m_paths || !m_paths->EnsureDataDir())
    {
        return false; // nowhere to write: nothing useful can even be reported
    }

    // Bootstrap order: parse the config, configure the logger from it, then replay what the
    // parse had to say. Nothing before this point can log, so it must not fail silently.
    config::ConfigLoadResult loaded = config::ConfigLoader::LoadOrCreate(m_paths->configFile);
    m_config = loaded.config;

    if (!logging::Initialize(m_config.logging, m_config.loader.console, m_paths->dataDir))
    {
        return false;
    }

    logging::LogStartup(fmt::format("Loader initialized (configuration: {}, log level: {})",
                                    Version::Configuration(),
                                    config::ToString(m_config.logging.level)));

    if (loaded.wroteDefault)
    {
        SPL_LOG_INFO(Config, "Wrote default configuration to '{}'",
                     util::ToUtf8(m_paths->configFile));
    }
    ReplayDiagnostics(loaded.diagnostics);

    if (m_config.loader.console && m_consoleInput.Start())
    {
        RegisterConsoleCommands(m_commands, *this);
        logging::LogCommandOutput("Console ready: type 'help' for the commands");
    }

    SPL_LOG_DEBUG(Core, "Game directory '{}'", util::ToUtf8(m_paths->gameDir));

    if (!m_config.loader.enabled)
    {
        logging::LogStartup("Loader disabled by configuration (loader.enabled = false)");
        return false; // ScriptMain exits; the ASI stays loaded and idle
    }

    // Before anything touches the game, so that a crash anywhere after this is reported.
    m_session = SessionGuard{m_paths->markerFile, m_paths->stateFile};
    const SessionStartup startup = m_session.Begin(m_config.loader.safeMode);
    const bool safeMode = ApplySessionStartup(startup);
    CrashHandler::Install(CrashHandler::Settings{
        .reportFile = m_paths->crashReportFile,
        .dumpFile = m_paths->crashDumpFile,
        .writeMinidump = m_config.diagnostics.writeMinidump,
        .describe = [this] { return DescribeForCrash(); },
        .onCrash =
            [this](const CrashReportInfo& info)
        {
            // Only a crash while game state was changing counts against a resource; one
            // during normal play gets a report and nothing more.
            if (m_session.IsBusy())
            {
                m_session.RecordCrash(
                    CrashMarker{.stage = info.stage, .resource = info.resource, .file = info.file});
            }
        }});

    const auto discoveryStart = std::chrono::steady_clock::now();
    m_resourcesRoot = m_paths->ResolveUserPath(m_config.paths.resources);
    m_resources.Discover(m_config, m_resourcesRoot, startup.quarantined);
    DiscoverMods(startup.quarantined);

    m_plan = streaming::StreamingPlan::Build(m_resources.GetResources(), m_config.streaming,
                                             m_config.diagnostics, m_config.dataFiles);
    m_discoveryMs = MillisecondsSince(discoveryStart);
    m_plan.LogSummary();

    if (safeMode)
    {
        m_state = LoaderState::SafeMode;
        SPL_LOG_WARNING(Core, "Safe mode: resources were read, but nothing is registered and no "
                              "game memory is touched this session");
    }

    m_bootstrap = BootstrapResult::Succeeded;
    return true;
}

EarlyStart Application::TryStartEarly()
{
    if (!Bootstrap() || m_state == LoaderState::SafeMode || !m_config.loader.earlyInit)
    {
        return EarlyStart::Declined;
    }
    if (!rage::InitHooks::IsGameCodeReady())
    {
        return EarlyStart::NotReady; // GTA5.exe has not decrypted its code yet
    }

    if (Result<void> resolved = m_bridge.Resolve(m_config); !resolved)
    {
        SPL_LOG_WARNING(Core, "Starting with story mode instead of with the game: {}",
                        resolved.GetMessage());
        return EarlyStart::Declined;
    }
    rage::InitHookCallbacks callbacks{
        .onInitialMount = [this] { OnGameMounted(); },
        .onPhaseStart = [this](rage::InitPhase phase) { OnInitPhaseStart(phase); },
        .onPhaseEnd = [this](rage::InitPhase phase) { OnInitPhaseEnd(phase); }};
    if (Result<void> installed =
            m_bridge.Init().Install(m_bridge.GetAddresses(), std::move(callbacks));
        !installed)
    {
        SPL_LOG_WARNING(Core, "Starting with story mode instead of with the game: {}",
                        installed.GetMessage());
        return EarlyStart::Declined;
    }

    m_startedEarly = true;
    SPL_LOG_INFO(Core, "Starting with the game (early_init)");
    if (!m_bridge.Init().HasInitialMountHook() && !m_modOverlays.empty())
    {
        SPL_LOG_WARNING(Mods, "Mod files are mounted only once the session starts, after the game "
                              "has read most of its own");
    }
    return EarlyStart::Started;
}

void Application::OnGameMounted()
{
    if (Result<void> devices = m_bridge.PrepareFileDevices(); !devices)
    {
        SPL_LOG_ERROR(Rage, "Resources and mods cannot be mounted while the game starts: {}",
                      devices.GetMessage());
        return;
    }
    const std::vector<std::string> gameReadMetas = FindMetasTheGameReads();
    MountGameRoots();
    if (!gameReadMetas.empty())
    {
        // The game reads these itself now that the overlays are in front of its own files, and
        // the same file loaded again as a data file would add every entry twice.
        const std::size_t removed = m_plan.RemoveDataFilesIf(
            [&](const streaming::PlannedDataFile& dataFile)
            {
                return std::ranges::find(gameReadMetas,
                                         util::ToUtf8Generic(dataFile.absolutePath)) !=
                       gameReadMetas.end();
            });
        SPL_LOG_DEBUG(Mods,
                      "{} mod data file(s) replace game files the game reads itself, so they "
                      "are not loaded again",
                      removed);
    }

    if (!m_bridge.Init().HasLevelMetaHooks())
    {
        WarnAboutLevelMetas();
        return;
    }
    m_bridge.Init().SetLevelMetas(CollectLevelMetas());
}

std::vector<std::string> Application::FindMetasTheGameReads() const
{
    std::vector<std::string> found;
    for (const streaming::PlannedDataFile& dataFile : m_plan.DataFiles())
    {
        const resource::Resource* const owner = m_resources.Find(dataFile.resourceName);
        if (owner == nullptr || !owner->IsMod())
        {
            continue;
        }
        // Only an overlay target has a game path: "common/data/x.meta" is "common:/data/x.meta".
        const std::size_t slash = dataFile.relativePath.find('/');
        const std::string_view folder = std::string_view{dataFile.relativePath}.substr(0, slash);
        if (slash == std::string::npos || (folder != "common" && folder != "platform"))
        {
            continue;
        }
        const std::string gamePath =
            fmt::format("{}:/{}", folder, dataFile.relativePath.substr(slash + 1));
        void* const device = m_bridge.Files().GetDevice(gamePath, true);
        if (device != nullptr && m_bridge.Files().GetFileAttributes(device, gamePath))
        {
            found.push_back(util::ToUtf8Generic(dataFile.absolutePath));
        }
    }
    return found;
}

void Application::OnInitPhaseStart(rage::InitPhase phase)
{
    // FiveM registers everything but maps as the session starts (LoadStreamingFile.cpp:3750).
    if (phase != rage::InitPhase::Session || m_connected)
    {
        return;
    }
    ConnectToGame(true);
    if (m_state != LoaderState::Ready)
    {
        return;
    }
    m_backend->SetInsideGameStartup(true);
    PumpStreaming(streaming::StreamingStage::GamePatches);
}

void Application::OnInitPhaseEnd(rage::InitPhase phase)
{
    logging::KeepConsoleVisible();
    // Maps, data files and the map store rebuild as the session ends (LoadStreamingFile.cpp:3768).
    if (phase != rage::InitPhase::Session || m_state != LoaderState::Ready ||
        m_sessionRegistrationDone)
    {
        return;
    }
    m_sessionRegistrationDone = true;
    PumpStreaming(streaming::StreamingStage::Done);
    m_backend->SetInsideGameStartup(false);
    m_streaming.ReassertRegistrations();
    CheckModOverlays();
    LogLoadSummary();
    if (rage::HasFaulted() && !m_faultHandled)
    {
        QuarantineFaultedResource();
    }
}

void Application::PumpStreaming(streaming::StreamingStage stop)
{
    for (std::size_t tick = 0; tick < kMaxStartupTicks && !m_streaming.IsFinished() &&
                               static_cast<int>(m_streaming.GetStage()) < static_cast<int>(stop);
         ++tick)
    {
        m_streaming.Tick();
    }
}

rage::LevelMetas Application::CollectLevelMetas() const
{
    rage::LevelMetas metas;
    const auto add = [this](const resource::Resource& resource,
                            const std::vector<std::string>& paths, std::vector<std::string>& out)
    {
        for (const std::string& path : paths)
        {
            std::optional<std::string> vfsPath =
                m_bridge.Files().ToVfsPath(resource.GetRootPath() / path);
            if (!vfsPath)
            {
                SPL_LOG_WARNING(Streaming, "{}: level meta '{}' is outside the mounted folders",
                                resource.GetName(), path);
                continue;
            }
            out.push_back(std::move(*vfsPath));
        }
    };
    for (const resource::Resource& resource : m_resources.GetResources())
    {
        const manifest::ResourceManifest* manifest = resource.GetManifest();
        if (!resource.IsEnabled() || manifest == nullptr)
        {
            continue;
        }
        add(resource, manifest->initMetas, metas.init);
        add(resource, manifest->beforeLevelMetas, metas.before);
        add(resource, manifest->afterLevelMetas, metas.after);
    }
    return metas;
}

void Application::WarnAboutLevelMetas() const
{
    for (const resource::Resource& resource : m_resources.GetResources())
    {
        const manifest::ResourceManifest* manifest = resource.GetManifest();
        if (resource.IsEnabled() && manifest != nullptr &&
            (!manifest->initMetas.empty() || !manifest->beforeLevelMetas.empty() ||
             !manifest->afterLevelMetas.empty()))
        {
            SPL_LOG_WARNING(Streaming,
                            "{}: its level metas are not loaded; they need the loader to start "
                            "with the game (loader.early_init)",
                            resource.GetName());
        }
    }
}

bool Application::ApplySessionStartup(const SessionStartup& startup)
{
    if (!startup.stateError.empty())
    {
        SPL_LOG_WARNING(Core, "{}", startup.stateError);
    }
    if (!startup.previousCrash)
    {
        return false;
    }

    const CrashMarker& crash = *startup.previousCrash;
    const std::string stage = crash.stage.empty() ? std::string{"registration"} : crash.stage;
    if (m_config.loader.safeMode == config::SafeMode::Off)
    {
        SPL_LOG_WARNING(Core,
                        "The previous session crashed during resource {}; loading everything "
                        "anyway (loader.safe_mode = \"off\")",
                        stage);
        return false;
    }
    if (!startup.newlyQuarantined.empty())
    {
        SPL_LOG_ERROR(Core,
                      "The previous session crashed during resource {} while '{}' was "
                      "registering{}; that resource is quarantined and everything else loads. "
                      "See crash.txt, and remove it from state.toml to try it again",
                      stage, startup.newlyQuarantined,
                      crash.file.empty() ? std::string{} : fmt::format(" ('{}')", crash.file));
        return false;
    }
    if (!crash.resource.empty())
    {
        return false; // already quarantined by an earlier crash; Discover() says so
    }

    SPL_LOG_ERROR(Core,
                  "The previous session crashed during resource {}; starting in safe mode. "
                  "Nothing is registered this time and the next launch is normal. The last "
                  "actions in crash.txt or resourceLoader.log name the resource to take out",
                  stage);
    return true;
}

void Application::DiscoverMods(std::span<const std::string> quarantined)
{
    RemoveOldModsCache(m_paths->dataDir);
    if (!m_config.mods.enabled)
    {
        return;
    }
    const std::filesystem::path modsFolder = m_paths->ResolveUserPath(m_config.paths.mods);
    mods::ModsScanner::Result scanned = mods::ModsScanner::Scan(modsFolder);
    for (const std::string& warning : scanned.warnings)
    {
        SPL_LOG_WARNING(Mods, warning);
    }
    if (scanned.mods.empty())
    {
        return;
    }

    const std::size_t discovered = scanned.mods.size();
    const mods::ModsScanner::Selection selection =
        mods::ModsScanner::Select(scanned.mods, m_config.mods);
    for (const std::string& name : selection.disabled)
    {
        SPL_LOG_DEBUG(Mods, "Mod '{}' disabled by configuration", name);
    }
    for (const std::string& name : selection.missingPriority)
    {
        SPL_LOG_WARNING(Mods, "Mods priority list names '{}', which was not found", name);
    }

    // The build number gates content DLCs the way FiveM's requiredVersion does. Read from
    // the executable itself: the bridge (and its build) does not exist this early.
    uint32_t gameBuild = 0;
    if (const std::optional<rage::GameBuild> detected = rage::DetectGameBuild())
    {
        gameBuild = detected->build;
    }

    // A mod's resource is rooted in the mods folder under its own name, a folder that does not
    // exist: its files are read from the archive next to it.
    std::vector<resource::ResourceCandidate> candidates;
    std::vector<std::pair<std::string, mods::ModLayout::Result>> laidOut;
    std::size_t mappedFiles = 0;
    for (const mods::DiscoveredMod& mod : scanned.mods)
    {
        mods::ModLayout::Result layout = mods::ModLayout::Build(mod, modsFolder, gameBuild);
        for (const std::string& warning : layout.warnings)
        {
            SPL_LOG_WARNING(Mods, warning);
        }
        candidates.push_back(
            resource::ResourceCandidate{.name = mod.name,
                                        .root = layout.root,
                                        .manifestPath = layout.root / "fxmanifest.lua",
                                        .manifestKind = resource::ManifestKind::FxManifest,
                                        .isMod = true,
                                        .files = layout.files});
        laidOut.emplace_back(mod.name, std::move(layout));
    }
    const std::vector<std::string> kept =
        m_resources.Adopt(m_config, std::move(candidates), quarantined);
    std::vector<mods::ModLayout::Result> keptResults;
    for (const std::string& name : kept)
    {
        SPL_LOG_DEBUG(Mods, "Found mod: {}", name);
        const auto mod =
            std::ranges::find(laidOut, name, [](const auto& pair) { return pair.first; });
        if (mod != laidOut.end())
        {
            m_modOverlays.insert(m_modOverlays.end(), mod->second.overlays.begin(),
                                 mod->second.overlays.end());
            mappedFiles += mod->second.files->GetFiles().size();
            m_modCatalog.Add(mod->second.files);
            keptResults.push_back(std::move(mod->second));
        }
    }
    // Only now is every half of a DLC split across mods known.
    for (const std::string& warning : mods::ModLayout::ReportUnresolvedDlcFiles(keptResults))
    {
        SPL_LOG_WARNING(Mods, warning);
    }
    if (!kept.empty())
    {
        m_modsRoot = modsFolder;
    }
    SPL_LOG_DEBUG(Mods,
                  "Discovered {} user mods ({} disabled, {} kept, {} files read from their "
                  "archives)",
                  discovered, selection.disabled.size(), kept.size(), mappedFiles);
}

void Application::ConnectToGame(bool insideGameStartup)
{
    m_connected = true;
    const auto bridgeStart = std::chrono::steady_clock::now();
    const Result<void> connected = m_bridge.Initialize(m_config);
    m_bridgeMs = MillisecondsSince(bridgeStart);
    if (!connected)
    {
        m_state = LoaderState::Degraded;
        SPL_LOG_ERROR(Rage, "RAGE bridge unavailable ({}): {}", rage::ToString(m_bridge.GetState()),
                      connected.GetMessage());
        SPL_LOG_WARNING(Core, "Running degraded: resources were read, but nothing will be "
                              "registered and no game memory is touched");
        return;
    }
    m_state = LoaderState::Ready;

    m_backend = std::make_unique<rage::RageStreamingBackend>(m_bridge, m_config);

    MountGameRoots();

    streaming::StreamingManager::Options options;
    if (insideGameStartup)
    {
        // The game waits for us inside its own load, so there is no frame to keep smooth.
        options.maxRegistrationsPerTick = std::numeric_limits<std::size_t>::max();
        options.maxDataFilesPerTick = std::numeric_limits<std::size_t>::max();
        options.maxManifestsPerTick = std::numeric_limits<std::size_t>::max();
        options.tickBudgetMicros = std::numeric_limits<int64_t>::max();
    }
    options.onBusyChanged = [this](bool busy)
    {
        if (busy)
        {
            m_session.MarkBusy(ToString(m_streaming.GetStage()));
        }
        else
        {
            m_session.MarkIdle();
        }
    };
    m_streaming.Start(m_plan, *m_backend, m_resourcesRoot, std::move(options));
}

void Application::MountGameRoots()
{
    if (m_rootsMounted)
    {
        return;
    }
    m_rootsMounted = true;
    if (Result<void> mounted = m_bridge.Files().MountResourcesRoot(m_resourcesRoot); !mounted)
    {
        SPL_LOG_ERROR(Rage, "The resources folder was not mounted: {}", mounted.GetMessage());
    }
    if (m_modsRoot.empty())
    {
        return;
    }
    // A failed mount leaves mod assets to fail registration one by one, each naming the missing
    // mount, rather than silently loading a half-modded game.
    if (Result<void> mounted = m_bridge.Files().MountModsRoot(m_modsRoot, m_modCatalog); !mounted)
    {
        SPL_LOG_ERROR(Mods, "The mods were not mounted: {}", mounted.GetMessage());
    }
    MountModOverlays();
}

void Application::CheckModOverlays() const
{
    for (const mods::ModLayout::OverlayRoot& overlay : m_modOverlays)
    {
        const std::string path = overlay.mountPoint + overlay.probeFile;
        if (m_bridge.IsReady() && !m_bridge.Files().IsServedByOverlay(path))
        {
            SPL_LOG_WARNING(Mods,
                            "'{}' no longer comes from '{}': a game device is in front of the "
                            "mod's files again",
                            path, util::ToUtf8(overlay.folder));
        }
    }
}

void Application::MountModOverlays()
{
    for (const mods::ModLayout::OverlayRoot& overlay : m_modOverlays)
    {
        const Result<bool> mounted = m_bridge.Files().MountOverlay(
            m_modCatalog, util::ToUtf8Generic(overlay.folder.lexically_relative(m_modsRoot)),
            overlay.mountPoint, overlay.probeFile);
        if (!mounted)
        {
            SPL_LOG_ERROR(Mods, "'{}' was not mounted over '{}': {}", util::ToUtf8(overlay.folder),
                          overlay.mountPoint, mounted.GetMessage());
            continue;
        }
        if (!mounted.GetValue())
        {
            SPL_LOG_WARNING(Mods,
                            "'{}' is mounted over '{}', but the game still serves '{}' from its "
                            "own files",
                            util::ToUtf8(overlay.folder), overlay.mountPoint, overlay.probeFile);
            continue;
        }
        SPL_LOG_DEBUG(Mods, "'{}' mounted over '{}'", util::ToUtf8Generic(overlay.folder),
                      overlay.mountPoint);
    }
    if (!m_modOverlays.empty() && !m_startedEarly)
    {
        // Started with story mode, after the game has read its startup files.
        SPL_LOG_INFO(Mods, "Mod files the game reads only while it starts (most .dat and .xml "
                           "settings) take effect only when the game reads them again; "
                           "loader.early_init mounts them in time");
    }
}

void Application::Tick()
{
    logging::KeepConsoleVisible();
    RunConsoleCommands();
    if (m_state != LoaderState::Ready)
    {
        return;
    }
    m_streaming.Tick();
    if (rage::HasFaulted() && !m_faultHandled)
    {
        QuarantineFaultedResource();
    }
    if (m_streaming.IsFinished())
    {
        LogLoadSummary();
    }
    if (m_streaming.GetStage() == streaming::StreamingStage::Done)
    {
        // The game's content change sets keep registering their own files for a while after
        // the session starts, story mode's scripts included.
        if (++m_ticksSinceReassert >= kReassertIntervalTicks)
        {
            m_ticksSinceReassert = 0;
            m_streaming.ReassertRegistrations();
        }
    }
}

void Application::RunConsoleCommands()
{
    for (const std::string& line : m_consoleInput.Queue().TakeAll())
    {
        logging::LogCommandOutput(fmt::format("> {}", line));
        for (const std::string& output : m_commands.Execute(line))
        {
            logging::LogCommandOutput(output);
        }
    }
}

void Application::QuarantineFaultedResource()
{
    m_faultHandled = true;
    const streaming::StreamingWork& work = m_streaming.GetCurrentWork();
    if (work.resourceName.empty())
    {
        return; // not per-resource work: the bridge stops, and nothing is anyone's fault
    }

    const std::string name{work.resourceName};
    m_session.Quarantine(name);
    if (resource::Resource* faulted = m_resources.Find(name))
    {
        faulted->SetState(resource::ResourceState::Failed, "a game call faulted");
    }
    SPL_LOG_ERROR(Core,
                  "A game call faulted while '{}' was registering '{}', so no further game calls "
                  "are made this session. The resource is quarantined from the next launch on; "
                  "remove it from state.toml to try it again",
                  name, work.fileName);
}

CrashReportInfo Application::DescribeForCrash() const
{
    CrashReportInfo info{.version = Version::Describe(),
                         .configuration = std::string{Version::Configuration()}};
    if (m_bridge.GetState() != rage::BridgeState::Uninitialized)
    {
        info.gameBuild = m_bridge.GetBuild().ToString();
    }

    const streaming::StreamingWork& work = m_streaming.GetCurrentWork();
    if (work.stage != streaming::StreamingStage::Idle)
    {
        info.stage = std::string{ToString(work.stage)};
    }
    info.resource = std::string{work.resourceName};
    info.file = std::string{work.fileName};

    for (const resource::Resource& resource : m_resources.GetResources())
    {
        info.resources.push_back(
            fmt::format("{} ({})", resource.GetName(), resource::ToString(resource.GetState())));
    }
    return info;
}

void Application::LogLoadSummary()
{
    if (m_summaryLogged)
    {
        return;
    }
    m_summaryLogged = true;

    std::vector<OverlayMount> overlays;
    for (const mods::ModLayout::OverlayRoot& overlay : m_modOverlays)
    {
        overlays.push_back(
            OverlayMount{.folder = overlay.folder, .mountPoint = overlay.mountPoint});
    }
    const LoadSummary summary =
        BuildLoadSummary(LoadSummaryInput{.resources = m_resources.GetResources(),
                                          .plan = &m_plan,
                                          .registry = &m_streaming.GetRegistry(),
                                          .overlays = overlays});
    for (const std::string& line : summary.resources)
    {
        SPL_LOG_INFO(Resource, "{}", line);
    }
    if (!summary.resourceTotals.empty())
    {
        SPL_LOG_INFO(Resource, "{}", summary.resourceTotals);
    }
    for (const std::string& line : summary.mods)
    {
        SPL_LOG_INFO(Mods, "{}", line);
    }
    if (!summary.modTotals.empty())
    {
        SPL_LOG_INFO(Mods, "{}", summary.modTotals);
    }

    int64_t activeMicros = 0;
    std::string stages;
    for (const streaming::StageTiming& timing : m_streaming.GetStageTimings())
    {
        activeMicros += timing.activeMicros;
        stages += stages.empty() ? "" : ", ";
        stages += DescribeTiming(timing);
    }
    const int64_t totalMs =
        m_discoveryMs + m_bridge.GetSignatures().durationMs + m_bridgeMs + activeMicros / 1000;
    std::string maps;
    switch (m_streaming.GetMapReloadResult())
    {
        using enum streaming::MapReloadResult;
    case Reloaded:
        maps = " (maps rebuilt)";
        break;
    case Failed:
        maps = " (maps could not be rebuilt)";
        break;
    case NotNeeded:
        break;
    }
    SPL_LOG_INFO(Streaming, "Ready in {:.1f} s{}", static_cast<double>(totalMs) / 1000.0, maps);
    if (const std::vector<std::size_t> slots = m_bridge.Files().GetUnexpectedModDeviceSlots();
        !slots.empty())
    {
        std::string list;
        for (const std::size_t slot : slots)
        {
            list += list.empty() ? fmt::format("{}", slot) : fmt::format(", {}", slot);
        }
        SPL_LOG_DEBUG(Mods, "The game called mod device slots nothing was known to call: {}", list);
    }
    SPL_LOG_DEBUG(Core, "Timings: discovery and plan {} ms, signatures {} ms, bridge {} ms; {}",
                  m_discoveryMs, m_bridge.GetSignatures().durationMs, m_bridgeMs, stages);
}

void Application::Shutdown()
{
    CrashHandler::Uninstall(); // its filter lives in this module
    m_consoleInput.Stop();
    if (m_initialized || m_startedEarly)
    {
        SPL_LOG_DEBUG(Core, "Shutting down");
        // The marker stays if registration was under way: a crash handler that ends the
        // process with ExitProcess also gets here, and quitting mid-registration is rare.
        // Hooks must go before the logger does: MinHook writes code, and a detour that runs
        // after this point would call into an unloaded module.
        hooking::HookManager::Instance().Shutdown();
        m_bridge.Init().RestorePatches();      // redirected calls into this module
        m_bridge.Patches().RestoreAll();       // a vtable slot points into this module
        m_bridge.Overrides().RestorePatches(); // so does a redirected call
        m_state = LoaderState::Uninitialized;
        m_initialized = false;
    }
    logging::Shutdown();
}
} // namespace spl
