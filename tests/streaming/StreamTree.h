#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "manifest/ResourceManifest.h"
#include "resource/Resource.h"
#include "tests/TempTree.h"

namespace spl::tests
{
/// The 16 bytes a compiled RAGE resource starts with, as they sit on disk. Synthesizing them
/// keeps binary fixtures out of the repository.
[[nodiscard]] inline std::string RscHeaderBytes(uint32_t version, uint32_t virtualFlags = 0,
                                                uint32_t physicalFlags = 0,
                                                uint32_t magic = 0x37435352)
{
    std::string bytes;
    const auto append = [&bytes](uint32_t word)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<char>((word >> shift) & 0xFF));
        }
    };

    append(magic);
    append(version);
    append(virtualFlags);
    append(physicalFlags);
    return bytes;
}

/// The start of a PSO metadata file ("PSIN" and a section size), which is what FiveM's
/// _manifest.ymf files are. Not a compiled resource.
[[nodiscard]] inline std::string PsoHeaderBytes()
{
    return std::string{"PSIN\0\0\0\x10", 8} + std::string(8, '\0');
}

/// A resources root whose resources have stream/ folders.
class StreamTree : public TempTree
{
public:
    /// Creates "<name>/fxmanifest.lua" and returns the resource that describes it.
    [[nodiscard]] resource::Resource AddStreamResource(std::string_view name) const
    {
        AddResource(name);
        return resource::Resource{
            resource::ResourceCandidate{.name = std::string{name},
                                        .root = Path() / name,
                                        .manifestPath = Path() / name / "fxmanifest.lua"}};
    }

    /// A file under "<resource>/stream/<relativePath>", with contents of its own.
    void AddFile(std::string_view resourceName, std::string_view relativePath,
                 std::string_view contents) const
    {
        WriteFile(std::string{resourceName} + "/stream/" + std::string{relativePath}, contents);
    }

    /// A file under stream/ that looks like a compiled resource: a valid header, no payload.
    void AddAsset(std::string_view resourceName, std::string_view relativePath, uint32_t version,
                  uint32_t virtualFlags = 0, uint32_t physicalFlags = 0) const
    {
        AddFile(resourceName, relativePath, RscHeaderBytes(version, virtualFlags, physicalFlags));
    }

    /// A file under stream/ that looks like a PSO metadata file.
    void AddPsoFile(std::string_view resourceName, std::string_view relativePath) const
    {
        AddFile(resourceName, relativePath, PsoHeaderBytes());
    }
};

/// A resource carrying a manifest, so the tests that care about this_is_a_map and data files
/// do not have to go through the parser.
[[nodiscard]] inline resource::Resource WithManifest(resource::Resource resource,
                                                     manifest::ResourceManifest manifest)
{
    resource.SetManifest(std::move(manifest));
    return resource;
}
} // namespace spl::tests
