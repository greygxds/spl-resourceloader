#include "core/ConsoleCommands.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/fmt/fmt.h>

#include "console/CommandRegistry.h"
#include "core/Application.h"
#include "rage/InteriorProxyPool.h"
#include "rage/Natives.h"
#include "rage/types/StreamingTypes.h"
#include "streaming/AssetRegistry.h"
#include "streaming/AssetType.h"
#include "util/Hash.h"
#include "util/Strings.h"

namespace spl
{
namespace
{
using console::Command;
using console::CommandOutput;

/// How far from the player `maps` lists interior proxies by default, in metres.
constexpr float kDefaultInteriorRadius = 150.0F;

[[nodiscard]] std::string_view ToString(rage::LoadState state)
{
    switch (state)
    {
        using enum rage::LoadState;
    case NotLoaded:
        return "not loaded";
    case Loaded:
        return "loaded";
    case Requested:
        return "requested";
    case Loading:
        return "loading";
    }
    return "unknown";
}

[[nodiscard]] std::string_view ResultText(const Result<void>& result)
{
    return result ? std::string_view{"available"} : result.GetMessage();
}

[[nodiscard]] bool IsMapAsset(streaming::AssetType type)
{
    using enum streaming::AssetType;
    return type == MapData || type == MapTypes || type == StaticBounds;
}

[[nodiscard]] std::string_view StemOf(std::string_view fileName)
{
    return fileName.substr(0, fileName.find_last_of('.'));
}

[[nodiscard]] std::optional<float> ParseFloat(std::string_view text)
{
    float value = 0.0F;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !(value > 0.0F))
    {
        return std::nullopt;
    }
    return value;
}

CommandOutput Status(Application& application)
{
    rage::RageBridge& bridge = application.GetBridge();
    const streaming::StreamingManager& streaming = application.GetStreaming();
    const streaming::AssetRegistry& registry = streaming.GetRegistry();

    CommandOutput output;
    output.push_back(fmt::format("Loader {}, bridge {}", ToString(application.GetState()),
                                 rage::ToString(bridge.GetState())));
    if (bridge.GetState() != rage::BridgeState::Uninitialized)
    {
        output.push_back(fmt::format("Game build {} ({})", bridge.GetBuild().ToString(),
                                     rage::ToString(bridge.GetBuildVerification())));
    }
    output.push_back(
        fmt::format("Streaming {}: {} asset(s) registered, {} override(s), {} data file(s) loaded",
                    streaming::ToString(streaming.GetStage()), registry.All().size(),
                    registry.CountOverrides(), registry.DataFiles().size()));
    output.push_back(fmt::format("Overrides: {}", ResultText(bridge.GetOverrideSupport())));
    output.push_back(
        fmt::format("Packfile manifests: {}", ResultText(bridge.GetManifestSupport())));
    output.push_back(fmt::format("Map store reload: {}", ResultText(bridge.GetMapReloadSupport())));
    return output;
}

CommandOutput Resources(Application& application)
{
    const streaming::StreamingPlan& plan = application.GetStreamingPlan();
    const streaming::AssetRegistry& registry = application.GetStreaming().GetRegistry();

    CommandOutput output;
    for (const resource::Resource& resource : application.GetResources().GetResources())
    {
        const auto resourcePlan =
            std::ranges::find(plan.Resources(), resource.GetName(), &streaming::ResourcePlan::name);
        const auto registered = std::ranges::count(registry.All(), resource.GetName(),
                                                   &streaming::RegisteredAsset::resourceName);
        const auto overrides = std::ranges::count_if(
            registry.All(), [&](const streaming::RegisteredAsset& asset)
            { return asset.resourceName == resource.GetName() && asset.overridesGameAsset; });
        const auto dataFiles = std::ranges::count(registry.DataFiles(), resource.GetName(),
                                                  &streaming::LoadedDataFile::resourceName);

        std::string line =
            fmt::format("  {:<28} {}", resource.GetName(), resource::ToString(resource.GetState()));
        if (!resource.GetStateReason().empty())
        {
            line += fmt::format(" ({})", resource.GetStateReason());
        }
        if (resourcePlan != plan.Resources().end())
        {
            line += fmt::format(": {} planned, {} skipped", resourcePlan->plannedAssets,
                                resourcePlan->skippedAssets);
        }
        line += fmt::format(", {} registered, {} override(s), {} data file(s)", registered,
                            overrides, dataFiles);
        output.push_back(std::move(line));
    }
    if (output.empty())
    {
        output.push_back("No resources");
    }
    else
    {
        output.insert(output.begin(), fmt::format("{} resource(s):", output.size()));
    }
    return output;
}

/// One registered asset and what the game's streaming entry for it says now.
[[nodiscard]] std::string DescribeAsset(Application& application,
                                        const streaming::RegisteredAsset& asset)
{
    std::string line = fmt::format("  {:<28} {:<24} {} index {} handle {:#010x}", asset.fileName,
                                   asset.resourceName, asset.moduleExtension,
                                   asset.globalIndex.value, asset.handle.value);
    rage::RageBridge& bridge = application.GetBridge();
    if (!bridge.IsReady())
    {
        return line + ", game state unreadable";
    }
    const std::optional<rage::StreamingDataEntry> entry =
        bridge.Streaming().GetEntry(asset.globalIndex);
    if (!entry)
    {
        return line + ", game state unreadable";
    }
    line += fmt::format(", {}", ToString(rage::LoadStateOf(*entry)));
    if (entry->handle != asset.handle.value)
    {
        line += fmt::format(", but the slot now carries {:#010x}", entry->handle);
    }
    if (asset.overridesGameAsset)
    {
        line += ", overrides a game asset";
    }
    return line;
}

CommandOutput Find(Application& application, std::string_view fileName)
{
    const std::string lowered = util::ToLower(fileName);
    const streaming::AssetRegistry& registry = application.GetStreaming().GetRegistry();
    if (const streaming::RegisteredAsset* const asset = registry.Find(lowered))
    {
        return {DescribeAsset(application, *asset)};
    }

    CommandOutput output;
    for (const streaming::StreamAsset& asset : application.GetStreamingPlan().Assets())
    {
        if (asset.fileName == lowered)
        {
            output.push_back(fmt::format(
                "  '{}' from '{}' is not registered: {}{}", asset.relativePath, asset.resourceName,
                streaming::ToString(asset.disposition),
                asset.dispositionReason.empty() ? std::string{}
                                                : fmt::format(" ({})", asset.dispositionReason)));
        }
    }
    if (output.empty())
    {
        output.push_back(fmt::format("No resource provides '{}'", lowered));
    }
    return output;
}

/// Requests or releases one registered asset through the streamer, then reports its state.
CommandOutput ChangeRequest(Application& application, std::string_view fileName, bool request)
{
    rage::RageBridge& bridge = application.GetBridge();
    if (!bridge.IsReady() || !bridge.Streaming().CanLoadObjects())
    {
        return {"The streamer's request functions are not available on this build"};
    }
    const std::string lowered = util::ToLower(fileName);
    const streaming::RegisteredAsset* const asset =
        application.GetStreaming().GetRegistry().Find(lowered);
    if (asset == nullptr)
    {
        return {
            fmt::format("'{}' is not registered by any resource; try 'find {}'", lowered, lowered)};
    }

    if (request)
    {
        // The request is kept: releasing it right away would let the streamer drop the object
        // before anyone can look at it. 'release' drops it.
        if (!bridge.Streaming().RequestObject(asset->globalIndex, 0) ||
            !bridge.Streaming().LoadAllRequestedObjects())
        {
            return {fmt::format("Requesting '{}' failed", lowered)};
        }
    }
    else if (!bridge.Streaming().ReleaseObject(asset->globalIndex))
    {
        return {fmt::format("The game kept '{}' (it is still referenced)", lowered)};
    }
    return {fmt::format("{} '{}':", request ? "Requested" : "Released", lowered),
            DescribeAsset(application, *asset)};
}

CommandOutput Maps(Application& application, float radius)
{
    rage::RageBridge& bridge = application.GetBridge();
    if (!bridge.IsReady())
    {
        return {fmt::format("The RAGE bridge is {}; map state cannot be read",
                            rage::ToString(bridge.GetState()))};
    }
    const streaming::AssetRegistry& registry = application.GetStreaming().GetRegistry();

    CommandOutput output{"Map assets:"};
    std::unordered_map<uint32_t, std::string> namesByHash;
    std::unordered_map<uint32_t, std::string> ourMapSlots;
    const std::optional<rage::StreamingModule> mapStore = bridge.Streaming().GetModule("ymap");
    for (const streaming::RegisteredAsset& asset : registry.All())
    {
        if (!IsMapAsset(asset.type))
        {
            continue;
        }
        output.push_back(DescribeAsset(application, asset));
        namesByHash.emplace(util::JoaatLower(StemOf(asset.fileName)), asset.fileName);
        if (asset.type == streaming::AssetType::MapData && mapStore)
        {
            ourMapSlots.emplace(asset.globalIndex.value - mapStore->BaseIndex(), asset.fileName);
        }
    }
    if (output.size() == 1)
    {
        output.push_back("  none registered");
    }

    const rage::WorldPosition player = rage::GetPlayerPosition();
    const std::optional<rage::InteriorProxySnapshot> snapshot = bridge.Interiors().Read();
    if (!snapshot)
    {
        output.push_back("Interior proxies: the pool is not readable on this build");
        return output;
    }
    output.push_back(
        fmt::format("Interior proxies: {} of {} in use; listing those within {} m of the player at "
                    "({:.1f}, {:.1f}, {:.1f}) or placed by a resource's .ymap",
                    snapshot->used, snapshot->capacity, radius, player.x, player.y, player.z));

    std::size_t listed = 0;
    for (const rage::InteriorProxyInfo& proxy : snapshot->proxies)
    {
        const float distance =
            std::hypot(proxy.x - player.x, proxy.y - player.y, proxy.z - player.z);
        const auto ours = ourMapSlots.find(proxy.mapDataSlot);
        if (distance > radius && ours == ourMapSlots.end())
        {
            continue;
        }
        const auto name = namesByHash.find(proxy.archetypeHash);
        output.push_back(fmt::format(
            "  #{:<5} archetype {:#010x}{} from ymap slot {}{} at ({:.1f}, {:.1f}, {:.1f}), {:.0f} "
            "m",
            proxy.poolIndex, proxy.archetypeHash,
            name != namesByHash.end() ? fmt::format(" ({})", StemOf(name->second)) : std::string{},
            proxy.mapDataSlot,
            ours != ourMapSlots.end() ? fmt::format(" ({})", ours->second) : std::string{}, proxy.x,
            proxy.y, proxy.z, distance));
        ++listed;
    }
    if (listed == 0)
    {
        output.push_back("  none");
    }
    return output;
}
} // namespace

void RegisterConsoleCommands(console::CommandRegistry& registry, Application& application)
{
    registry.Add(Command{.name = "status",
                         .usage = "status",
                         .summary = "loader, bridge and streaming state",
                         .maxArguments = 0,
                         .run = [&application](std::span<const std::string>)
                         { return Status(application); }});
    registry.Add(Command{.name = "resources",
                         .usage = "resources",
                         .summary = "every resource with what was planned, registered and loaded",
                         .maxArguments = 0,
                         .run = [&application](std::span<const std::string>)
                         { return Resources(application); }});
    registry.Add(Command{.name = "find",
                         .usage = "find <file>",
                         .summary = "where a stream file went, and what the game says about it now",
                         .minArguments = 1,
                         .maxArguments = 1,
                         .run = [&application](std::span<const std::string> arguments)
                         { return Find(application, arguments.front()); }});
    registry.Add(Command{.name = "request",
                         .usage = "request <file>",
                         .summary = "loads a registered asset now and keeps it loaded",
                         .minArguments = 1,
                         .maxArguments = 1,
                         .run = [&application](std::span<const std::string> arguments)
                         { return ChangeRequest(application, arguments.front(), true); }});
    registry.Add(Command{.name = "release",
                         .usage = "release <file>",
                         .summary = "drops what 'request' loaded",
                         .minArguments = 1,
                         .maxArguments = 1,
                         .run = [&application](std::span<const std::string> arguments)
                         { return ChangeRequest(application, arguments.front(), false); }});
    registry.Add(Command{
        .name = "maps",
        .usage = "maps [radius]",
        .summary = "registered .ymap/.ytyp/.ybn states, and interior proxies near the player",
        .maxArguments = 1,
        .run = [&application](std::span<const std::string> arguments) -> CommandOutput
        {
            std::optional<float> radius = kDefaultInteriorRadius;
            if (!arguments.empty())
            {
                radius = ParseFloat(arguments.front());
            }
            if (!radius)
            {
                return {"Usage: maps [radius], with the radius in metres"};
            }
            return Maps(application, *radius);
        }});
}
} // namespace spl
