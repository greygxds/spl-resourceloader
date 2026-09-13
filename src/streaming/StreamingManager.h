#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "streaming/AssetRegistry.h"
#include "streaming/StreamingBackend.h"
#include "streaming/StreamingPlan.h"

namespace spl::streaming
{
/// Where the pump is. The order is the order the stages run in, and every stage is entered
/// exactly once.
enum class StreamingStage
{
    Idle, ///< nothing started yet
    Mount,
    RegisterEarly,
    GamePatches, ///< only does anything when the plan has .ytyp or .ymap files
    RegisterLate,
    DataFiles,
    Manifests,         ///< .ymf packfile manifests
    MapReload,         ///< waits for a safe moment, then rebuilds the map store once
    DeferredDataFiles, ///< GTXD_PARENTING_DATA, which FiveM mounts after the rebuild
    Done,
    Failed ///< the mount did not come up, so nothing can be registered
};

[[nodiscard]] std::string_view ToString(StreamingStage stage);

struct RegistrationTotals
{
    std::size_t registered = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;

    /// Planned, but of a type whose registration is not written yet. The plan still lists
    /// them, because what the loader intends to do does not change with the version.
    std::size_t deferred = 0;
};

struct DataFileTotals
{
    std::size_t loaded = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
};

struct ManifestTotals
{
    std::size_t loaded = 0;
    std::size_t failed = 0;
    std::size_t releasedTypeRequests = 0;
};

/// Where the pump spent its time in one stage, for the startup summary.
struct StageTiming
{
    StreamingStage stage = StreamingStage::Idle;
    int64_t activeMicros = 0; ///< time inside Tick(), not the wall time between ticks
    std::size_t ticks = 0;
    int64_t slowestTickMicros = 0;
};

/// What the pump is doing at this moment, for breadcrumbs and the crash report. The views point
/// into the plan, which outlives the manager.
struct StreamingWork
{
    StreamingStage stage = StreamingStage::Idle;
    std::string_view resourceName; ///< empty outside per-resource work
    std::string_view fileName;
};

/// What the MapReload stage did, for the summary and for tests.
enum class MapReloadResult
{
    NotNeeded, ///< no resource has maps, collisions or this_is_a_map (or the stage has not run)
    Reloaded,
    Failed
};

/// Drives registration across ticks. Every step is bounded, because it runs on the game's
/// main thread: a plan with thousands of assets must not turn into one multi-second frame.
///
/// Pure code. It talks to the game only through IStreamingBackend, which is what lets the
/// ordering, the batching and the failure handling be unit-tested against a fake.
class StreamingManager
{
public:
    /// A batch this size takes well under a frame in practice.
    static constexpr std::size_t kMaxRegistrationsPerTick = 512;

    /// A data file is read and parsed synchronously by the game, so far fewer fit in a frame.
    static constexpr std::size_t kMaxDataFilesPerTick = 16;

    /// A manifest is parsed and committed synchronously, and each one can pull in many maps.
    static constexpr std::size_t kMaxManifestsPerTick = 4;

    /// A tick stops starting new work once it has run this long, whatever the batch sizes
    /// allow. Roughly half a frame at 60 fps.
    static constexpr int64_t kTickBudgetMicros = 8000;

    struct Options
    {
        std::size_t maxRegistrationsPerTick = kMaxRegistrationsPerTick;
        std::size_t maxDataFilesPerTick = kMaxDataFilesPerTick;
        std::size_t maxManifestsPerTick = kMaxManifestsPerTick;
        int64_t tickBudgetMicros = kTickBudgetMicros;

        /// Microseconds from any fixed point. Empty uses the steady clock; tests pass their own.
        std::function<int64_t()> clockMicros;

        /// Called with true before a tick changes game state, and with false once the pump is
        /// waiting or finished. Only called when the value changes.
        std::function<void(bool busy)> onBusyChanged;
    };

    /// Neither plan nor backend is copied, so both must outlive the manager. Calling Start
    /// again restarts the pump and forgets what was registered before.
    void Start(const StreamingPlan& plan, IStreamingBackend& backend,
               std::filesystem::path resourcesRoot, Options options = {});

    /// One bounded step. Does nothing once the pump has finished.
    void Tick();

    /// Once registration is done: puts back every registration the game has replaced with its
    /// own file since, and returns how many. Cheap enough to call every few ticks.
    std::size_t ReassertRegistrations();

    [[nodiscard]] StreamingStage GetStage() const
    {
        return m_stage;
    }

    [[nodiscard]] bool IsFinished() const
    {
        return m_stage == StreamingStage::Done || m_stage == StreamingStage::Failed;
    }

    [[nodiscard]] const AssetRegistry& GetRegistry() const
    {
        return m_registry;
    }

    [[nodiscard]] const RegistrationTotals& GetTotals() const
    {
        return m_totals;
    }

    [[nodiscard]] const DataFileTotals& GetDataFileTotals() const
    {
        return m_dataFileTotals;
    }

    [[nodiscard]] const ManifestTotals& GetManifestTotals() const
    {
        return m_manifestTotals;
    }

    [[nodiscard]] MapReloadResult GetMapReloadResult() const
    {
        return m_mapReloadResult;
    }

    /// One entry per stage that ran, in the order they ran.
    [[nodiscard]] const std::vector<StageTiming>& GetStageTimings() const
    {
        return m_timings;
    }

    [[nodiscard]] const StreamingWork& GetCurrentWork() const
    {
        return m_work;
    }

private:
    void RunMount();

    /// Registers up to maxRegistrationsPerTick of assets, continuing where the last tick
    /// stopped. Moves on to next once the span is exhausted.
    void RunRegistrations(std::span<const PlannedAsset> assets, StreamingStage next);

    /// Installs the map-types patches when a late asset needs them. A refusal is logged and the
    /// pump goes on: the types still register, they may just not place.
    void RunGamePatches();

    /// Loads up to maxDataFilesPerTick data files, continuing where the last tick stopped.
    /// Moves on to next once the span is exhausted.
    void RunDataFiles(std::span<const PlannedDataFile> dataFiles, StreamingStage next);

    /// Hands the backend what it has to finish once the regular data files are in.
    void FinishDataFiles();

    /// Loads up to maxManifestsPerTick packfile manifests, continuing where the last tick stopped.
    void RunManifests();

    /// Rebuilds the map store once the backend says it is safe, or skips it when no resource
    /// needs it. Stays in the stage while it waits.
    void RunMapReload();

    /// Goes on to the deferred data files, or finishes when there are none.
    void FinishMapStage();

    void Finish();

    /// The debug line for one registration, with the game's copy it replaced if any.
    void LogRegistration(const PlannedAsset& asset, const RegistrationOutcome& outcome) const;

    void Enter(StreamingStage stage);

    /// Records what is about to run, and leaves a breadcrumb for the crash report.
    void BeginWork(std::string_view resourceName, std::string_view fileName,
                   std::string_view action);
    void SetBusy(bool busy);
    [[nodiscard]] int64_t NowMicros() const;

    /// True once this tick has used its time budget. Always false for the first item of a
    /// tick, so a single slow asset cannot stall the pump.
    [[nodiscard]] bool IsOverBudget(std::size_t doneThisTick) const;
    void LogStageSummary(std::string_view stageName, const RegistrationTotals& totals) const;

    const StreamingPlan* m_plan = nullptr;
    IStreamingBackend* m_backend = nullptr;
    std::filesystem::path m_resourcesRoot;
    Options m_options;

    StreamingStage m_stage = StreamingStage::Idle;
    std::size_t m_cursor = 0;         ///< index into the stage's asset span
    RegistrationTotals m_stageTotals; ///< reset when a registration stage is entered
    RegistrationTotals m_totals;      ///< across every stage
    DataFileTotals m_dataFileTotals;
    ManifestTotals m_manifestTotals;
    MapReloadResult m_mapReloadResult = MapReloadResult::NotNeeded;
    bool m_mapReloadWaitLogged = false;
    bool m_busy = false;
    int64_t m_tickStartMicros = 0;
    StreamingWork m_work;
    std::vector<StageTiming> m_timings;
    AssetRegistry m_registry;
};
} // namespace spl::streaming
