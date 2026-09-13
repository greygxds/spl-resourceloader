#include "resource/ResourceScanner.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "resource/Resource.h"
#include "util/Strings.h"

namespace spl::resource
{
namespace
{
constexpr std::string_view kFxManifestName = "fxmanifest.lua";
constexpr std::string_view kLegacyManifestName = "__resource.lua";

/// A directory still to be walked, with the categories that enclose it.
struct PendingDirectory
{
    std::filesystem::path path;
    std::vector<std::string> categories;
};

struct FoundManifest
{
    std::filesystem::path path;
    ManifestKind kind;
};

/// fxmanifest.lua wins when both are present, so modernizing a resource is a matter of adding
/// the new file (FiveM behaves the same way).
std::optional<FoundManifest> FindManifest(const std::filesystem::path& directory,
                                          bool acceptLegacyManifest)
{
    std::error_code error;

    const std::filesystem::path fxManifest = directory / kFxManifestName;
    if (std::filesystem::is_regular_file(fxManifest, error))
    {
        return FoundManifest{.path = fxManifest, .kind = ManifestKind::FxManifest};
    }

    if (acceptLegacyManifest)
    {
        const std::filesystem::path legacy = directory / kLegacyManifestName;
        if (std::filesystem::is_regular_file(legacy, error))
        {
            return FoundManifest{.path = legacy, .kind = ManifestKind::LegacyResourceLua};
        }
    }
    return std::nullopt;
}

/// A category directory that also holds a manifest is almost always a resource whose author
/// wrapped the name in brackets by mistake, so it is worth saying so (FiveM warns too).
bool HasAnyManifest(const std::filesystem::path& directory)
{
    std::error_code error;
    return std::filesystem::is_regular_file(directory / kFxManifestName, error) ||
           std::filesystem::is_regular_file(directory / kLegacyManifestName, error);
}

/// One directory level, sorted case-insensitively by name so that discovery order, and with it
/// which of two duplicates wins, never depends on how the filesystem enumerates.
std::vector<std::filesystem::path> ListSortedDirectories(const std::filesystem::path& directory,
                                                         std::vector<std::string>& warnings)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    if (error)
    {
        warnings.push_back(
            fmt::format("Cannot read '{}': {}", util::ToUtf8(directory), error.message()));
        return {};
    }

    std::vector<std::filesystem::path> directories;
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        std::error_code entryError;
        if (entry.is_directory(entryError))
        {
            directories.push_back(entry.path());
        }
    }

    std::ranges::sort(directories,
                      [](const std::filesystem::path& left, const std::filesystem::path& right)
                      {
                          return util::ToLower(util::ToUtf8(left.filename())) <
                                 util::ToLower(util::ToUtf8(right.filename()));
                      });
    return directories;
}
} // namespace

bool IsCategoryName(std::string_view name)
{
    return name.size() >= 2 && name.front() == '[' && name.back() == ']';
}

bool IsPlainResourceName(std::string_view name)
{
    return !name.empty() && std::ranges::all_of(name,
                                                [](char character)
                                                {
                                                    return (character >= 'A' && character <= 'Z') ||
                                                           (character >= 'a' && character <= 'z') ||
                                                           (character >= '0' && character <= '9') ||
                                                           character == '_' || character == '-' ||
                                                           character == '.';
                                                });
}

ResourceScanner::Result ResourceScanner::Scan(const std::filesystem::path& root,
                                              bool acceptLegacyManifest)
{
    Result result;

    std::queue<PendingDirectory> pending;
    pending.push(PendingDirectory{.path = root, .categories = {}});

    // Canonical paths of directories already walked, so a junction pointing at an ancestor
    // cannot send the walk round in circles.
    std::set<std::filesystem::path> visited;

    // Lower-case name to the path that claimed it: the first resource of a given name wins.
    std::unordered_map<std::string, std::filesystem::path> claimed;

    while (!pending.empty())
    {
        const PendingDirectory current = std::move(pending.front());
        pending.pop();

        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(current.path, error);
        if (!visited.insert(error ? current.path : canonical).second)
        {
            continue; // already walked: a symlink or junction loop
        }

        for (const std::filesystem::path& directory :
             ListSortedDirectories(current.path, result.warnings))
        {
            const std::string name = util::ToUtf8(directory.filename());

            // Hidden folders, which covers .git and the like.
            if (name.starts_with('.'))
            {
                continue;
            }

            if (IsCategoryName(name))
            {
                if (HasAnyManifest(directory))
                {
                    result.warnings.push_back(
                        fmt::format("'{}' is a category but has a resource manifest; drop the "
                                    "brackets if it is meant to be a resource",
                                    name));
                }

                std::vector<std::string> categories = current.categories;
                categories.push_back(name);
                pending.push(
                    PendingDirectory{.path = directory, .categories = std::move(categories)});
                continue;
            }

            const std::optional<FoundManifest> manifest =
                FindManifest(directory, acceptLegacyManifest);
            if (!manifest)
            {
                // Not a resource, and a resource's own subfolders are never searched, so this
                // branch deliberately does not recurse.
                SPL_LOG_DEBUG(Resource, "Skipping '{}' (no manifest)", name);
                continue;
            }

            const std::string lowered = util::ToLower(name);
            if (const auto existing = claimed.find(lowered); existing != claimed.end())
            {
                result.warnings.push_back(
                    fmt::format("Duplicate resource '{}': using '{}', ignoring '{}'", name,
                                util::ToUtf8(existing->second), util::ToUtf8(directory)));
                continue;
            }
            claimed.emplace(lowered, directory);

            if (!IsPlainResourceName(name))
            {
                result.warnings.push_back(
                    fmt::format("Resource name '{}' has characters outside A-Z, 0-9, '_', '-' "
                                "and '.', which may not map cleanly onto game paths",
                                name));
            }

            result.resources.push_back(ResourceCandidate{.name = name,
                                                         .root = directory,
                                                         .manifestPath = manifest->path,
                                                         .manifestKind = manifest->kind,
                                                         .categories = current.categories});
        }
    }

    return result;
}
} // namespace spl::resource
