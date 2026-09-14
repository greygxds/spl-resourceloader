#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "manifest/ResourceManifest.h"
#include "util/FileTree.h"

namespace spl::resource
{
/// A resource's position in ResourceManager's vector. Other subsystems keep ids rather than
/// pointers into it.
struct ResourceId
{
    uint16_t value = 0;

    auto operator<=>(const ResourceId&) const = default;
};

enum class ManifestKind
{
    FxManifest,       ///< fxmanifest.lua
    LegacyResourceLua ///< __resource.lua, accepted when resources.accept_legacy_manifest
};

[[nodiscard]] std::string_view ToString(ManifestKind kind);

/// Where a resource is in its lifecycle.
enum class ResourceState
{
    Discovered,    ///< found on disk
    Disabled,      ///< filtered out by configuration
    ManifestError, ///< the manifest could not be read
    Scanned,       ///< stream assets enumerated
    Registered,    ///< every eligible asset is known to the game
    PartiallyRegistered,
    Failed
};

[[nodiscard]] std::string_view ToString(ResourceState state);

/// What the scanner found on disk, before any configuration is applied.
struct ResourceCandidate
{
    std::string name;           ///< folder name; compared case-insensitively
    std::filesystem::path root; ///< absolute
    std::filesystem::path manifestPath;
    ManifestKind manifestKind = ManifestKind::FxManifest;
    std::vector<std::string> categories; ///< enclosing "[maps]" folders, for diagnostics
    bool isMod = false;                  ///< user mod: logs as a mod, sorts last

    /// Where the resource's files are read from; the disk when null. A user mod's root names no
    /// folder, and its files come from its archive.
    std::shared_ptr<const util::IFileTree> files;
};

/// One resource, with the state it has reached. Owned by ResourceManager.
class Resource
{
public:
    explicit Resource(ResourceCandidate candidate);

    [[nodiscard]] const std::string& GetName() const
    {
        return m_candidate.name;
    }
    [[nodiscard]] const std::filesystem::path& GetRootPath() const
    {
        return m_candidate.root;
    }
    [[nodiscard]] const std::filesystem::path& GetManifestPath() const
    {
        return m_candidate.manifestPath;
    }
    [[nodiscard]] ManifestKind GetManifestKind() const
    {
        return m_candidate.manifestKind;
    }
    [[nodiscard]] const std::vector<std::string>& GetCategories() const
    {
        return m_candidate.categories;
    }

    /// The tree every path under GetRootPath() is read through.
    [[nodiscard]] const util::IFileTree& GetFiles() const
    {
        return m_candidate.files ? *m_candidate.files : util::DiskFileTree::Instance();
    }

    /// <root>/stream. It may not exist; a resource without one is still a valid resource.
    [[nodiscard]] std::filesystem::path GetStreamPath() const;
    [[nodiscard]] bool HasStreamDirectory() const;

    /// A user mod rather than a resource folder.
    [[nodiscard]] bool IsMod() const
    {
        return m_candidate.isMod;
    }

    [[nodiscard]] ResourceState GetState() const
    {
        return m_state;
    }
    [[nodiscard]] const std::string& GetStateReason() const
    {
        return m_stateReason;
    }
    void SetState(ResourceState state, std::string reason = {});

    /// The parsed manifest, or nullptr until ResourceManager has read it (and for a resource
    /// whose manifest could not be parsed).
    [[nodiscard]] const manifest::ResourceManifest* GetManifest() const
    {
        return m_manifest.get();
    }
    void SetManifest(manifest::ResourceManifest manifest);

    /// True for a resource the loader will act on: anything that is not Disabled or Failed.
    [[nodiscard]] bool IsEnabled() const;

private:
    ResourceCandidate m_candidate;
    std::unique_ptr<manifest::ResourceManifest> m_manifest;
    ResourceState m_state = ResourceState::Discovered;
    std::string m_stateReason;
};
} // namespace spl::resource
