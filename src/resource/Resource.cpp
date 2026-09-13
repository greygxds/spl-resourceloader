#include "resource/Resource.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace spl::resource
{
namespace
{
constexpr std::string_view kStreamFolderName = "stream";
} // namespace

std::string_view ToString(ManifestKind kind)
{
    using enum ManifestKind;
    switch (kind)
    {
    case FxManifest:
        return "fxmanifest.lua";
    case LegacyResourceLua:
        return "__resource.lua";
    }
    return "fxmanifest.lua";
}

std::string_view ToString(ResourceState state)
{
    using enum ResourceState;
    switch (state)
    {
    case Discovered:
        return "discovered";
    case Disabled:
        return "disabled";
    case ManifestError:
        return "manifest error";
    case Scanned:
        return "scanned";
    case Registered:
        return "registered";
    case PartiallyRegistered:
        return "partially registered";
    case Failed:
        return "failed";
    }
    return "discovered";
}

Resource::Resource(ResourceCandidate candidate) : m_candidate(std::move(candidate)) {}

std::filesystem::path Resource::GetStreamPath() const
{
    return m_candidate.root / kStreamFolderName;
}

bool Resource::HasStreamDirectory() const
{
    std::error_code error;
    return std::filesystem::is_directory(GetStreamPath(), error);
}

void Resource::SetState(ResourceState state, std::string reason)
{
    m_state = state;
    m_stateReason = std::move(reason);
}

void Resource::SetManifest(manifest::ResourceManifest manifest)
{
    m_manifest = std::make_unique<manifest::ResourceManifest>(std::move(manifest));
}

bool Resource::IsEnabled() const
{
    return m_state != ResourceState::Disabled && m_state != ResourceState::Failed;
}
} // namespace spl::resource
