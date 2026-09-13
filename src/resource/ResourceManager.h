#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/LoaderConfig.h"
#include "resource/Resource.h"

namespace spl::resource
{
/// Owns every discovered resource, in load order.
///
/// The vector is built once per discovery and then not resized, so other subsystems can hold
/// indices into it.
class ResourceManager
{
public:
    /// Scans resolvedRoot, applies the configuration's filters and ordering, and logs what it
    /// found. Creates the root when it is missing, so users can see where resources go.
    /// Resources named in quarantined (state.toml) are skipped like disabled ones,
    /// with a warning that says how to let them load again.
    void Discover(const config::LoaderConfig& config, const std::filesystem::path& resolvedRoot,
                  std::span<const std::string> quarantined = {});

    /// Appends pre-built candidates (user mods laid out from their archives) with the same filters
    /// and manifest loading as discovery. They sort after everything discovered, so with the
    /// default duplicate policy a same-named file loses to the resource's. A name that is
    /// already taken is skipped with an error: names share one namespace because
    /// quarantine looks them up. Returns the kept names, in order.
    [[nodiscard]] std::vector<std::string> Adopt(const config::LoaderConfig& config,
                                                 std::vector<ResourceCandidate> candidates,
                                                 std::span<const std::string> quarantined = {});

    /// In load order, disabled resources included.
    [[nodiscard]] std::span<Resource> GetResources()
    {
        return m_resources;
    }
    [[nodiscard]] std::span<const Resource> GetResources() const
    {
        return m_resources;
    }

    /// Case-insensitive lookup. nullptr when no resource has that name; the pointer is
    /// non-owning and stays valid until the next Discover().
    [[nodiscard]] Resource* Find(std::string_view name);
    [[nodiscard]] const Resource* Find(std::string_view name) const;

    [[nodiscard]] std::size_t CountEnabled() const;

private:
    /// Reads and parses one resource's manifest, setting ManifestError or Disabled when it
    /// cannot be used. Diagnostics are logged as "<resource>:<line>: <message>".
    void LoadManifest(Resource& resource);

    /// Appends one candidate with the standard filters. False when the name is taken.
    bool AddResource(const config::LoaderConfig& config, ResourceCandidate candidate,
                     std::span<const std::string> quarantined);

    std::vector<Resource> m_resources;
};
} // namespace spl::resource
