#include "streaming/AssetRegistry.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "streaming/AssetType.h"
#include "util/Hash.h"

namespace spl::streaming
{
namespace
{
constexpr std::string_view kItypRequestType = "DLC_ITYP_REQUEST";

[[nodiscard]] std::string_view StreamingNameOf(std::string_view fileName)
{
    const std::size_t dot = fileName.find_last_of('.');
    return dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
}
} // namespace

void AssetRegistry::Add(RegisteredAsset asset)
{
    m_assets.push_back(std::move(asset));
}

void AssetRegistry::AddDataFile(LoadedDataFile dataFile)
{
    m_dataFiles.push_back(std::move(dataFile));
}

bool AssetRegistry::MarkDataFileReleased(std::string_view entryName)
{
    const auto match = std::ranges::find(m_dataFiles, entryName, &LoadedDataFile::entryName);
    if (match == m_dataFiles.end())
    {
        return false;
    }
    match->released = true;
    return true;
}

void AssetRegistry::AddMapDependency(MapDependency dependency)
{
    m_mapDependencies.push_back(dependency);
}

void AssetRegistry::PushHandle(rage::GlobalIndex index, rage::StreamingHandle ours,
                               std::optional<rage::StreamingHandle> replaced)
{
    HandleStack& stack = m_handleStacks[index];
    if (!stack.gameHandle && stack.loaderHandles.empty())
    {
        stack.gameHandle = replaced;
    }
    stack.loaderHandles.push_back(ours);
}

void AssetRegistry::RecordDisplacedGameHandle(rage::GlobalIndex index,
                                              rage::StreamingHandle gameHandle)
{
    m_handleStacks[index].gameHandle = gameHandle;
    for (RegisteredAsset& asset : m_assets)
    {
        if (asset.globalIndex == index)
        {
            asset.overridesGameAsset = true;
        }
    }
}

std::optional<rage::StreamingHandle> AssetRegistry::PopHandle(rage::GlobalIndex index,
                                                              rage::StreamingHandle ours)
{
    const auto found = m_handleStacks.find(index);
    if (found == m_handleStacks.end())
    {
        return std::nullopt;
    }

    HandleStack& stack = found->second;
    std::erase(stack.loaderHandles, ours);
    if (!stack.loaderHandles.empty())
    {
        return stack.loaderHandles.back();
    }

    const std::optional<rage::StreamingHandle> gameHandle = stack.gameHandle;
    m_handleStacks.erase(found);
    return gameHandle;
}

const HandleStack* AssetRegistry::FindHandleStack(rage::GlobalIndex index) const
{
    const auto found = m_handleStacks.find(index);
    return found != m_handleStacks.end() ? &found->second : nullptr;
}

std::size_t AssetRegistry::CountOverrides() const
{
    return static_cast<std::size_t>(
        std::ranges::count(m_assets, true, &RegisteredAsset::overridesGameAsset));
}

const RegisteredAsset* AssetRegistry::Find(std::string_view fileName) const
{
    // File names are lower-cased when the asset is discovered, so a plain compare is enough.
    const auto match = std::ranges::find(m_assets, fileName, &RegisteredAsset::fileName);
    return match != m_assets.end() ? &*match : nullptr;
}

std::size_t AssetRegistry::CountOf(AssetType type) const
{
    return static_cast<std::size_t>(std::ranges::count(m_assets, type, &RegisteredAsset::type));
}

std::vector<RegisteredAsset> AssetRegistry::OfType(AssetType type) const
{
    std::vector<RegisteredAsset> matches;
    std::ranges::copy_if(m_assets, std::back_inserter(matches),
                         [type](const RegisteredAsset& asset) { return asset.type == type; });
    return matches;
}

void AssetRegistry::Clear()
{
    m_assets.clear();
    m_dataFiles.clear();
    m_mapDependencies.clear();
    m_handleStacks.clear();
}

const LoadedDataFile* FindTypeRequest(std::span<const LoadedDataFile> dataFiles,
                                      uint32_t mapTypesHash)
{
    const auto match = std::ranges::find_if(
        dataFiles,
        [mapTypesHash](const LoadedDataFile& dataFile)
        {
            return !dataFile.released && dataFile.type == kItypRequestType &&
                   util::JoaatLower(StreamingNameOf(dataFile.fileName)) == mapTypesHash;
        });
    return match != dataFiles.end() ? &*match : nullptr;
}
} // namespace spl::streaming
