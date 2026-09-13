#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mods/ModFileTree.h"
#include "rage/ModArchiveDevice.h"

namespace spl::mods
{
/// Every adopted mod's files by their "<mod folder>/<mod-relative path>", which is what follows
/// "splmods:/" in a VFS path. The source the ModArchiveDevices serve.
class ModCatalog final : public rage::IDeviceFileSource
{
public:
    /// Adds a mod under the name of its root folder. A second tree with the same folder name,
    /// compared case-insensitively, is ignored: resource names are unique already.
    void Add(std::shared_ptr<const ModFileTree> tree);

    [[nodiscard]] bool IsEmpty() const
    {
        return m_trees.empty();
    }

    [[nodiscard]] std::optional<rage::DeviceFileInfo>
    FindFile(std::string_view path) const override;
    [[nodiscard]] bool IsDirectory(std::string_view path) const override;
    [[nodiscard]] std::optional<rage::DeviceFileBytes>
    OpenFile(std::string_view path) const override;
    [[nodiscard]] std::vector<rage::DeviceDirectoryEntry>
    List(std::string_view path) const override;

private:
    struct Located
    {
        const ModFileTree* tree = nullptr; ///< nullptr when no mod has the folder
        std::string relative;              ///< the path inside that mod
    };
    [[nodiscard]] Located Locate(std::string_view path) const;

    std::unordered_map<std::string, std::shared_ptr<const ModFileTree>>
        m_trees;                        ///< by lower folder
    std::vector<std::string> m_folders; ///< as added, for listing the root
};
} // namespace spl::mods
