#include "resource/ResourceManager.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "config/LoaderConfig.h"
#include "logging/Logger.h"
#include "manifest/ManifestParser.h"
#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "resource/ResourceScanner.h"
#include "util/Strings.h"

namespace spl::resource
{
namespace
{
bool ContainsIgnoreCase(const std::vector<std::string>& names, std::string_view name)
{
    return std::ranges::any_of(names, [&](const std::string& candidate)
                               { return util::EqualsIgnoreCase(candidate, name); });
}

/// Makes sure the resources root exists, so a first-time user has somewhere obvious to put
/// resources. Returns false when it is missing and cannot be created.
bool EnsureRootExists(const std::filesystem::path& root)
{
    std::error_code error;
    if (std::filesystem::is_directory(root, error))
    {
        return true;
    }

    std::filesystem::create_directories(root, error);
    if (error)
    {
        SPL_LOG_ERROR(Resource, "Cannot create the resources folder '{}': {}", util::ToUtf8(root),
                      error.message());
        return false;
    }

    SPL_LOG_INFO(Resource, "Resources folder '{}' did not exist and was created",
                 util::ToUtf8(root));
    return false; // freshly created, so there is nothing in it to discover
}

/// Load order: the priority entries first, in the order the user listed them, then everything
/// else by name. Order decides duplicate_policy and override precedence,
/// so it has to be stable and explainable.
void SortIntoLoadOrder(std::vector<ResourceCandidate>& candidates,
                       const std::vector<std::string>& priority)
{
    const auto priorityIndex = [&priority](const ResourceCandidate& candidate) -> std::size_t
    {
        for (std::size_t index = 0; index < priority.size(); ++index)
        {
            if (util::EqualsIgnoreCase(priority[index], candidate.name))
            {
                return index;
            }
        }
        return priority.size(); // everything unlisted sorts after everything listed
    };

    std::ranges::stable_sort(candidates,
                             [&](const ResourceCandidate& left, const ResourceCandidate& right)
                             {
                                 const std::size_t leftIndex = priorityIndex(left);
                                 const std::size_t rightIndex = priorityIndex(right);
                                 if (leftIndex != rightIndex)
                                 {
                                     return leftIndex < rightIndex;
                                 }
                                 return util::ToLower(left.name) < util::ToLower(right.name);
                             });
}
} // namespace

void ResourceManager::Discover(const config::LoaderConfig& config,
                               const std::filesystem::path& resolvedRoot,
                               std::span<const std::string> quarantined)
{
    m_resources.clear();

    SPL_LOG_INFO(Resource, "Resources root: {}", util::ToUtf8Generic(resolvedRoot));
    if (!EnsureRootExists(resolvedRoot))
    {
        return;
    }

    ResourceScanner::Result scan =
        ResourceScanner::Scan(resolvedRoot, config.resources.acceptLegacyManifest);
    for (const std::string& warning : scan.warnings)
    {
        SPL_LOG_WARNING(Resource, warning);
    }

    for (const std::string& wanted : config.resources.priority)
    {
        const bool present =
            std::ranges::any_of(scan.resources, [&](const ResourceCandidate& candidate)
                                { return util::EqualsIgnoreCase(candidate.name, wanted); });
        if (!present)
        {
            SPL_LOG_WARNING(Resource, "Priority list names '{}', which was not found", wanted);
        }
    }

    SortIntoLoadOrder(scan.resources, config.resources.priority);

    m_resources.reserve(scan.resources.size());
    for (ResourceCandidate& candidate : scan.resources)
    {
        AddResource(config, std::move(candidate), quarantined);
    }

    for (Resource& resource : m_resources)
    {
        if (resource.IsEnabled())
        {
            LoadManifest(resource);
        }
    }

    const std::size_t enabled = CountEnabled();
    SPL_LOG_DEBUG(Resource, "Discovered {} resources ({} enabled, {} disabled)", m_resources.size(),
                  enabled, m_resources.size() - enabled);
}

std::vector<std::string> ResourceManager::Adopt(const config::LoaderConfig& config,
                                                std::vector<ResourceCandidate> candidates,
                                                std::span<const std::string> quarantined)
{
    std::vector<std::string> kept;
    if (candidates.empty())
    {
        return kept;
    }
    m_resources.reserve(m_resources.size() + candidates.size());
    const std::size_t first = m_resources.size();
    for (ResourceCandidate& candidate : candidates)
    {
        if (AddResource(config, std::move(candidate), quarantined))
        {
            kept.push_back(m_resources.back().GetName());
        }
    }
    for (std::size_t index = first; index < m_resources.size(); ++index)
    {
        if (m_resources[index].IsEnabled())
        {
            LoadManifest(m_resources[index]);
        }
    }
    return kept;
}

bool ResourceManager::AddResource(const config::LoaderConfig& config, ResourceCandidate candidate,
                                  std::span<const std::string> quarantined)
{
    const std::string name = candidate.name;
    if (Find(name) != nullptr)
    {
        SPL_LOG_ERROR(Resource, "{} '{}' skipped: the name is already taken",
                      candidate.isMod ? "Mod" : "Resource", name);
        return false;
    }

    const bool isMod = candidate.isMod;
    const std::string_view manifestKind = ToString(candidate.manifestKind);

    // " [maps] [old]", or empty for a resource sitting directly in the root.
    std::string categories;
    for (const std::string& category : candidate.categories)
    {
        categories += ' ';
        categories += category;
    }

    Resource& resource = m_resources.emplace_back(std::move(candidate));

    // Mods have their own disabled and priority lists, applied before they are extracted.
    if (!isMod && ContainsIgnoreCase(config.resources.disabled, name))
    {
        resource.SetState(ResourceState::Disabled, "disabled by configuration");
        SPL_LOG_DEBUG(Resource, "Resource '{}' disabled by configuration", name);
        return true;
    }

    const bool isQuarantined =
        std::ranges::any_of(quarantined, [&](const std::string& candidateName)
                            { return util::EqualsIgnoreCase(candidateName, name); });
    if (isQuarantined)
    {
        resource.SetState(ResourceState::Disabled, "quarantined after a crash");
        SPL_LOG_WARNING(Resource,
                        "Resource '{}' is quarantined because the game crashed while it was "
                        "being registered; remove it from state.toml to try it again",
                        name);
        return true;
    }

    // With auto_discover off, the priority list is the whole list: nothing else loads.
    if (!isMod && !config.resources.autoDiscover &&
        !ContainsIgnoreCase(config.resources.priority, name))
    {
        resource.SetState(ResourceState::Disabled, "not in priority list");
        SPL_LOG_DEBUG(Resource, "Resource '{}' disabled (not in priority list)", name);
        return true;
    }

    if (!isMod)
    {
        SPL_LOG_DEBUG(Resource, "Found resource: {} ({}){}", name, manifestKind, categories);
    }
    return true;
}

void ResourceManager::LoadManifest(Resource& resource)
{
    const std::filesystem::path& path = resource.GetManifestPath();

    std::ifstream stream{path, std::ios::binary};
    if (!stream)
    {
        resource.SetState(ResourceState::ManifestError, "manifest could not be opened");
        SPL_LOG_ERROR(Resource, "Cannot read the manifest of '{}' ('{}')", resource.GetName(),
                      util::ToUtf8(path));
        return;
    }

    const std::string source{std::istreambuf_iterator<char>{stream},
                             std::istreambuf_iterator<char>{}};

    const std::string chunkName = util::ToUtf8(path.filename());
    manifest::ParseResult parsed = manifest::ManifestParser::Parse(source, chunkName);

    manifest::ResourceManifest typed = manifest::ResourceManifest::FromDocument(
        parsed.document, resource.GetRootPath(), parsed.diagnostics);

    for (const manifest::ManifestDiagnostic& diagnostic : parsed.diagnostics)
    {
        using Severity = manifest::ManifestDiagnostic::Severity;
        const std::string where =
            fmt::format("{}:{}:{}", resource.GetName(), diagnostic.line, diagnostic.column);
        switch (diagnostic.severity)
        {
        case Severity::Error:
            SPL_LOG_ERROR(Manifest, "{}: {}", where, diagnostic.message);
            break;
        case Severity::Warning:
            SPL_LOG_WARNING(Manifest, "{}: {}", where, diagnostic.message);
            break;
        case Severity::Info:
            SPL_LOG_DEBUG(Manifest, "{}: {}", where, diagnostic.message);
            break;
        }
    }

    if (parsed.fatal)
    {
        resource.SetState(ResourceState::ManifestError, "manifest could not be parsed");
        return;
    }

    if (!typed.IsCompatibleWithGta5())
    {
        const std::string games = typed.DescribeGames();
        resource.SetState(ResourceState::Disabled, fmt::format("manifest targets {}", games));
        SPL_LOG_DEBUG(Resource, "Resource '{}' disabled (manifest targets {})", resource.GetName(),
                      games);
        return;
    }

    if (typed.ignoredScriptEntries > 0)
    {
        SPL_LOG_DEBUG(Manifest, "'{}': ignoring {} script entries (scripts are not supported)",
                      resource.GetName(), typed.ignoredScriptEntries);
    }

    SPL_LOG_DEBUG(Manifest, "'{}': this_is_a_map: {}, {} data file(s)", resource.GetName(),
                  typed.isMap ? "yes" : "no", typed.dataFiles.size());

    resource.SetManifest(std::move(typed));
}

Resource* ResourceManager::Find(std::string_view name)
{
    const auto match =
        std::ranges::find_if(m_resources, [&](const Resource& resource)
                             { return util::EqualsIgnoreCase(resource.GetName(), name); });
    return match != m_resources.end() ? &*match : nullptr;
}

const Resource* ResourceManager::Find(std::string_view name) const
{
    const auto match =
        std::ranges::find_if(m_resources, [&](const Resource& resource)
                             { return util::EqualsIgnoreCase(resource.GetName(), name); });
    return match != m_resources.end() ? &*match : nullptr;
}

std::size_t ResourceManager::CountEnabled() const
{
    return static_cast<std::size_t>(std::ranges::count_if(m_resources, [](const Resource& resource)
                                                          { return resource.IsEnabled(); }));
}
} // namespace spl::resource
