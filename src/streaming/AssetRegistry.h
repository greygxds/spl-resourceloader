#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rage/types/StreamingTypes.h"
#include "resource/Resource.h"
#include "streaming/AssetType.h"

namespace spl::streaming
{
/// One asset the game now knows about: what undoing a registration would need, and what the
/// map-store reload needs to tell which collisions changed.
struct RegisteredAsset
{
    resource::ResourceId owner;
    std::string resourceName;
    std::string vfsPath;         ///< "splres:/map_one/stream/prop.ydr"
    std::string fileName;        ///< "prop.ydr", the name the game knows it by
    std::string moduleExtension; ///< the asset store it landed in: "ytd"
    AssetType type = AssetType::Unknown;

    rage::GlobalIndex globalIndex;
    rage::StreamingHandle handle;

    /// The slot already carried a game asset's handle, which this one replaced. The handle to
    /// put back lives in the registry's handle stack, not here.
    bool overridesGameAsset = false;
};

/// The handles one global index has carried since the loader first touched it.
struct HandleStack
{
    /// What the slot carried before the loader took it over; empty for a slot we created.
    std::optional<rage::StreamingHandle> gameHandle;

    /// Ours, oldest first. The last one is what the slot carries now.
    std::vector<rage::StreamingHandle> loaderHandles;
};

/// A data file the game's mounter accepted.
struct LoadedDataFile
{
    resource::ResourceId owner;
    std::string resourceName;
    std::string type;      ///< "DLC_ITYP_REQUEST"
    std::string fileName;  ///< "props.ytyp"
    std::string entryName; ///< the name the mounter was handed, which unloading needs again

    /// A packfile manifest took the file over, so the mounter no longer holds it.
    bool released = false;
};

/// "This .ymap needs this .ytyp", as a resource's packfile manifest declared it, so maps
/// can be unloaded in the right order.
struct MapDependency
{
    resource::ResourceId owner;
    uint32_t mapDataHash = 0;
    uint32_t mapTypesHash = 0;
};

/// Everything the loader has registered, in registration order. It owns no game state: it is
/// the record of what was done, and what undoing it would read.
class AssetRegistry
{
public:
    void Add(RegisteredAsset asset);

    void AddDataFile(LoadedDataFile dataFile);

    [[nodiscard]] std::span<const LoadedDataFile> DataFiles() const
    {
        return m_dataFiles;
    }

    /// Marks the data file loaded under entryName as released. False when there is none.
    bool MarkDataFileReleased(std::string_view entryName);

    void AddMapDependency(MapDependency dependency);

    [[nodiscard]] std::span<const MapDependency> MapDependencies() const
    {
        return m_mapDependencies;
    }

    /// Records that index now carries ours. replaced is the game's own handle, kept only the
    /// first time: a second resource overriding the same index stacks on top of the first, and
    /// the game's handle stays the one to restore (FiveM LoadStreamingFile.cpp:1935).
    void PushHandle(rage::GlobalIndex index, rage::StreamingHandle ours,
                    std::optional<rage::StreamingHandle> replaced = std::nullopt);

    /// The game registered its own file in index after we did, and ours was put back on top:
    /// gameHandle is now what lies underneath, and every asset at index overrides it.
    void RecordDisplacedGameHandle(rage::GlobalIndex index, rage::StreamingHandle gameHandle);

    /// Takes ours off index's stack, wherever it sits in it. Returns the handle the slot should
    /// carry from now on: the newest of ours that is left, else the game's own. std::nullopt
    /// means nothing is left and the slot has to be cleared (FiveM LoadStreamingFile.cpp:2620).
    [[nodiscard]] std::optional<rage::StreamingHandle> PopHandle(rage::GlobalIndex index,
                                                                 rage::StreamingHandle ours);

    /// What index carried and carries, or nullptr when the loader never touched it.
    /// Non-owning: invalid after the next PushHandle, PopHandle or Clear.
    [[nodiscard]] const HandleStack* FindHandleStack(rage::GlobalIndex index) const;

    /// How many registered assets replaced a game asset.
    [[nodiscard]] std::size_t CountOverrides() const;

    [[nodiscard]] std::span<const RegisteredAsset> All() const
    {
        return m_assets;
    }

    /// The asset registered under fileName, or nullptr. Non-owning: the pointer is invalid
    /// after the next Add().
    [[nodiscard]] const RegisteredAsset* Find(std::string_view fileName) const;

    [[nodiscard]] std::size_t Size() const
    {
        return m_assets.size();
    }

    [[nodiscard]] std::size_t CountOf(AssetType type) const;

    /// Every registered asset of one type, copied, for the map-store reload.
    [[nodiscard]] std::vector<RegisteredAsset> OfType(AssetType type) const;

    void Clear();

private:
    std::vector<RegisteredAsset> m_assets;
    std::vector<LoadedDataFile> m_dataFiles;
    std::vector<MapDependency> m_mapDependencies;
    std::map<rage::GlobalIndex, HandleStack> m_handleStacks;
};

/// The unreleased DLC_ITYP_REQUEST whose .ytyp a packfile manifest names by mapTypesHash, the
/// JoaatLower of its streaming name. nullptr when none matches. Non-owning.
[[nodiscard]] const LoadedDataFile* FindTypeRequest(std::span<const LoadedDataFile> dataFiles,
                                                    uint32_t mapTypesHash);
} // namespace spl::streaming
