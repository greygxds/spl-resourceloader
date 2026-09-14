#include "streaming/RscHeader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

#include "core/Result.h"
#include "util/FileTree.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
/// Magic words as they read on disk, little-endian: 'R' 'S' 'C' '7' is 0x37435352.
constexpr uint32_t kMagicRsc7 = 0x37435352;
constexpr uint32_t kMagicRsc8 = 0x38435352;
constexpr uint32_t kMagicRsc5 = 0x05435352;

/// 'P' 'S' 'I' 'N', the first section of a PSO file. FiveM's _manifest.ymf files are PSO
/// (CPackFileMetaData), not compiled resources.
constexpr uint32_t kMagicPso = 0x4E495350;

constexpr std::size_t kHeaderSizeBytes = 16;

/// One page count inside a page-flag word: mask bits out at shift, then every page it counts
/// is 1 << weightShift base pages large.
struct PageField
{
    uint32_t shift;
    uint32_t mask;
    uint32_t weightShift;
};

/// Re-implementation of the decode in FiveM's ConvertRSC7Size
/// (citizen-server-impl/src/ResourceStreamComponent.cpp:404-419): nine page counts, from the
/// largest page (bit 27) down to the smallest (bit 4).
constexpr std::array<PageField, 9> kPageFields{{
    {.shift = 27, .mask = 0x1, .weightShift = 0},
    {.shift = 26, .mask = 0x1, .weightShift = 1},
    {.shift = 25, .mask = 0x1, .weightShift = 2},
    {.shift = 24, .mask = 0x1, .weightShift = 3},
    {.shift = 17, .mask = 0x7F, .weightShift = 4},
    {.shift = 11, .mask = 0x3F, .weightShift = 5},
    {.shift = 7, .mask = 0xF, .weightShift = 6},
    {.shift = 5, .mask = 0x3, .weightShift = 7},
    {.shift = 4, .mask = 0x1, .weightShift = 8},
}};

/// The low four bits scale the base page, which is 512 bytes at shift 0.
constexpr uint32_t kBasePageSizeBytes = 0x200;
constexpr uint32_t kPageScaleMask = 0xF;

uint32_t ReadLittleEndian32(const std::array<char, kHeaderSizeBytes>& bytes, std::size_t offset)
{
    const auto byteAt = [&bytes](std::size_t index)
    { return static_cast<uint32_t>(static_cast<unsigned char>(bytes[index])); };

    return byteAt(offset) | (byteAt(offset + 1) << 8) | (byteAt(offset + 2) << 16) |
           (byteAt(offset + 3) << 24);
}

std::optional<RscHeader::Format> FormatOf(uint32_t magic)
{
    switch (magic)
    {
    case kMagicRsc7:
        return RscHeader::Format::Rsc7;
    case kMagicRsc8:
        return RscHeader::Format::Rsc8;
    case kMagicRsc5:
        return RscHeader::Format::Rsc5;
    default:
        return std::nullopt;
    }
}
} // namespace

uint64_t RscHeader::VirtualSizeBytes() const
{
    return DecodeRsc7PageFlags(virtualFlags);
}

uint64_t RscHeader::PhysicalSizeBytes() const
{
    return DecodeRsc7PageFlags(physicalFlags);
}

uint64_t DecodeRsc7PageFlags(uint32_t flags)
{
    uint64_t basePages = 0;
    for (const PageField& field : kPageFields)
    {
        basePages += static_cast<uint64_t>((flags >> field.shift) & field.mask)
                     << field.weightShift;
    }

    const uint32_t scale = flags & kPageScaleMask;
    return basePages * (static_cast<uint64_t>(kBasePageSizeBytes) << scale);
}

RscHeader ParseRscHeader(std::string_view bytes)
{
    if (bytes.size() < kHeaderSizeBytes)
    {
        return RscHeader{}; // too short to hold a header, so it cannot be a resource
    }
    std::array<char, kHeaderSizeBytes> header{};
    std::copy_n(bytes.data(), kHeaderSizeBytes, header.data());

    const uint32_t magic = ReadLittleEndian32(header, 0);
    if (magic == kMagicPso)
    {
        return RscHeader{.format = RscHeader::Format::Pso};
    }

    const std::optional<RscHeader::Format> format = FormatOf(magic);
    if (!format)
    {
        return RscHeader{};
    }

    return RscHeader{.format = *format,
                     .version = ReadLittleEndian32(header, 4),
                     .virtualFlags = ReadLittleEndian32(header, 8),
                     .physicalFlags = ReadLittleEndian32(header, 12)};
}

std::optional<RscHeader> ReadRscHeader(const std::filesystem::path& file, std::string* error)
{
    return ReadRscHeader(util::DiskFileTree::Instance(), file, error);
}

std::optional<RscHeader> ReadRscHeader(const util::IFileTree& files,
                                       const std::filesystem::path& file, std::string* error)
{
    const Result<std::string> bytes = files.Read(file, kHeaderSizeBytes);
    if (!bytes)
    {
        if (error != nullptr)
        {
            *error = bytes.GetMessage();
        }
        return std::nullopt;
    }
    return ParseRscHeader(bytes.GetValue());
}

std::string_view ToString(RscHeader::Format format)
{
    switch (format)
    {
        using enum RscHeader::Format;
    case None:
        return "none";
    case Rsc7:
        return "RSC7";
    case Rsc8:
        return "RSC8";
    case Rsc5:
        return "RSC5";
    case Pso:
        return "PSIN";
    }
    return "none";
}
} // namespace spl::streaming
