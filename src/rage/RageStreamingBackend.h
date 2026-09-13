#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "rage/RageBridge.h"
#include "streaming/AssetRegistry.h"
#include "streaming/StreamingBackend.h"
#include "streaming/StreamingPlan.h"

namespace spl::rage
{
/// The real backend: it mounts the resources folder and hands assets to the game's raw
/// streamer. Everything it does goes through RageBridge, so a faulted bridge turns every
/// call into a reported failure rather than a crash.
class RageStreamingBackend final : public streaming::IStreamingBackend
{
public:
    RageStreamingBackend(RageBridge& bridge, const config::LoaderConfig& config)
        : m_bridge(&bridge), m_settings(config.streaming),
          m_mapReloadStrategy(config.diagnostics.mapReloadStrategy)
    {
    }

    [[nodiscard]] Result<void> PrepareResourceRoot(const std::filesystem::path& root) override;
    [[nodiscard]] streaming::RegistrationOutcome
    RegisterAsset(const streaming::PlannedAsset& asset) override;

    [[nodiscard]] Result<void> InstallMapTypesPatches() override;
    [[nodiscard]] Result<void> InstallMapDataPatches() override;
    [[nodiscard]] Result<std::string>
    LoadDataFile(const streaming::PlannedDataFile& dataFile) override;
    void FinishDataFiles(const streaming::DataFileFinishRequest& request) override;
    [[nodiscard]] Result<streaming::PackfileManifestOutcome>
    LoadPackfileManifest(const streaming::PlannedManifest& manifest,
                         std::span<const streaming::LoadedDataFile> loadedDataFiles) override;
    [[nodiscard]] bool CanReloadMapStore() override;
    [[nodiscard]] Result<streaming::MapReloadReport>
    ReloadMapStore(std::span<const streaming::RegisteredAsset> collisions) override;
    [[nodiscard]] std::optional<streaming::ReassertOutcome>
    ReassertAsset(const streaming::RegisteredAsset& asset) override;
    [[nodiscard]] Result<void> UnregisterAsset(const streaming::RegisteredAsset& asset) override;

    /// True while the pump runs inside the game's own startup, where no native may
    /// be called and the map store is rebuilt without waiting for the loading screen.
    void SetInsideGameStartup(bool inside)
    {
        m_insideGameStartup = inside;
    }

private:
    [[nodiscard]] Result<void> RequireReadyBridge() const;

    /// Takes over slot, which already carries the game's handle.
    [[nodiscard]] streaming::RegistrationOutcome
    RegisterOverride(const streaming::PlannedAsset& asset, const StreamingModule& module,
                     LocalSlot slot, const StreamingDataEntry& entry, const std::string& vfsPath);

    /// Makes sure a loaded game copy does not outlive the override longer than it must.
    [[nodiscard]] streaming::OverrideTiming ReleaseGameCopy(const StreamingModule& module,
                                                            LocalSlot slot, GlobalIndex index,
                                                            const StreamingDataEntry& entry);

    /// "platform:/textures/<file name>" when the game keeps a core texture dictionary of that
    /// name there, which is the name its slot has (FiveM LoadStreamingFile.cpp:1731).
    [[nodiscard]] std::optional<std::string> FindCoreTexturePath(std::string_view fileName) const;

    /// A permanent .ytyp the game loaded from its own files is released before a data file
    /// loads our replacement, or the request would find it already loaded (FiveM
    /// LoadStreamingFile.cpp:1320).
    void ReleasePermanentMapTypes(std::string_view fileName);

    /// Store slots per module. Counting a pool walks every entry, so it happens
    /// once per module and registrations keep the count up to date from then on.
    struct SlotBudget
    {
        uint32_t size = 0;
        uint32_t used = 0;
    };

    /// nullptr when the pool cannot be read, which leaves the check to the game.
    [[nodiscard]] SlotBudget* FindSlotBudget(const StreamingModule& module);

    RageBridge* m_bridge;
    std::unordered_map<uintptr_t, SlotBudget> m_slotBudgets;
    config::StreamingSettings m_settings;
    config::MapReloadStrategy m_mapReloadStrategy;
    bool m_insideGameStartup = false;
};
} // namespace spl::rage
