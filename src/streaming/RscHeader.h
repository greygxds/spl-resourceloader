#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "util/FileTree.h"

namespace spl::streaming
{
/// The 16-byte header every compiled RAGE resource starts with: magic, resource version and
/// the two page-flag words that say how much virtual and physical memory it needs.
struct RscHeader
{
    enum class Format : uint8_t
    {
        None, ///< the file has no RSC magic: it is not a compiled resource
        Rsc7,
        Rsc8,
        Rsc5,
        Pso ///< "PSIN": PSO metadata such as a _manifest.ymf. Not a compiled resource
    };

    Format format = Format::None;
    uint32_t version = 0; ///< resource type version, e.g. 13 for a .ytd
    uint32_t virtualFlags = 0;
    uint32_t physicalFlags = 0;

    [[nodiscard]] uint64_t VirtualSizeBytes() const;
    [[nodiscard]] uint64_t PhysicalSizeBytes() const;

    /// True for a file that really is a compiled resource, whatever the magic's flavor.
    [[nodiscard]] bool IsResource() const
    {
        return format != Format::None && format != Format::Pso;
    }

    /// True for a PSO metadata file ("PSIN"). The fields after the magic mean nothing then.
    [[nodiscard]] bool IsPsoMetadata() const
    {
        return format == Format::Pso;
    }
};

/// Reads the header of file.
///
/// std::nullopt means the file could not be read, and *error (when non-null) says why. A file
/// that reads fine but carries no RSC magic gives a header with Format::None, because "this is
/// not a compiled resource" is an answer, not a failure. A PSO file gives Format::Pso and
/// nothing else.
[[nodiscard]] std::optional<RscHeader> ReadRscHeader(const std::filesystem::path& file,
                                                     std::string* error = nullptr);

/// The same, read through a file tree.
[[nodiscard]] std::optional<RscHeader> ReadRscHeader(const util::IFileTree& files,
                                                     const std::filesystem::path& file,
                                                     std::string* error = nullptr);

/// The header at the start of bytes; Format::None when there is no RSC or PSO magic, or fewer
/// than 16 bytes.
[[nodiscard]] RscHeader ParseRscHeader(std::string_view bytes);

/// Decodes one RSC7 page-flag word into bytes. The word packs nine page counts, each page
/// twice the size of the next, plus a 4-bit shift that scales all of them.
[[nodiscard]] uint64_t DecodeRsc7PageFlags(uint32_t flags);

[[nodiscard]] std::string_view ToString(RscHeader::Format format);
} // namespace spl::streaming
