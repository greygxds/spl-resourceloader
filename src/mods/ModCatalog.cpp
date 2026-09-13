#include "mods/ModCatalog.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mods/ModFileTree.h"
#include "rage/ModArchiveDevice.h"
#include "util/Strings.h"

namespace spl::mods
{
void ModCatalog::Add(std::shared_ptr<const ModFileTree> tree)
{
    std::string folder = util::ToUtf8(tree->GetRoot().filename());
    if (m_trees.try_emplace(util::ToLower(folder), std::move(tree)).second)
    {
        m_folders.push_back(std::move(folder));
    }
}

ModCatalog::Located ModCatalog::Locate(std::string_view path) const
{
    const std::size_t start = path.find_first_not_of("/\\");
    if (start == std::string_view::npos)
    {
        return {};
    }
    path.remove_prefix(start);
    const std::size_t slash = path.find_first_of("/\\");
    const auto found = m_trees.find(util::ToLower(path.substr(0, slash)));
    if (found == m_trees.end())
    {
        return {};
    }
    return Located{.tree = found->second.get(),
                   .relative = slash == std::string_view::npos
                                   ? std::string{}
                                   : std::string{path.substr(slash + 1)}};
}

std::optional<rage::DeviceFileInfo> ModCatalog::FindFile(std::string_view path) const
{
    const Located located = Locate(path);
    const ModFile* const file =
        located.tree != nullptr ? located.tree->FindFile(located.relative) : nullptr;
    return file != nullptr ? located.tree->DescribeForGame(*file) : std::nullopt;
}

bool ModCatalog::IsDirectory(std::string_view path) const
{
    if (path.find_first_not_of("/\\") == std::string_view::npos)
    {
        return true; // the root, which lists the mods
    }
    const Located located = Locate(path);
    return located.tree != nullptr && located.tree->IsDirectory(located.relative);
}

std::optional<rage::DeviceFileBytes> ModCatalog::OpenFile(std::string_view path) const
{
    const Located located = Locate(path);
    const ModFile* const file =
        located.tree != nullptr ? located.tree->FindFile(located.relative) : nullptr;
    return file != nullptr ? located.tree->OpenForGame(*file) : std::nullopt;
}

std::vector<rage::DeviceDirectoryEntry> ModCatalog::List(std::string_view path) const
{
    if (path.find_first_not_of("/\\") == std::string_view::npos)
    {
        std::vector<rage::DeviceDirectoryEntry> mods;
        for (const std::string& folder : m_folders)
        {
            mods.push_back(rage::DeviceDirectoryEntry{.name = folder, .isDirectory = true});
        }
        std::ranges::sort(mods, {}, [](const rage::DeviceDirectoryEntry& entry)
                          { return util::ToLower(entry.name); });
        return mods;
    }
    const Located located = Locate(path);
    return located.tree != nullptr ? located.tree->ListForGame(located.relative)
                                   : std::vector<rage::DeviceDirectoryEntry>{};
}
} // namespace spl::mods
