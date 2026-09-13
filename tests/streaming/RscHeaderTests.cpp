#include <cstdint>
#include <optional>
#include <string>

#include <catch_amalgamated.hpp>

#include "streaming/RscHeader.h"
#include "tests/streaming/StreamTree.h"

using spl::streaming::DecodeRsc7PageFlags;
using spl::streaming::ReadRscHeader;
using spl::streaming::RscHeader;
using spl::tests::RscHeaderBytes;
using spl::tests::TempDir;

namespace
{
/// The base page is 512 bytes at shift 0, so every expectation below is a page count times
/// this (or times a shifted version of it).
constexpr uint64_t kBasePage = 512;
} // namespace

TEST_CASE("RscHeader: decodes page flags into bytes", "[streaming]")
{
    // No pages at all.
    REQUIRE(DecodeRsc7PageFlags(0) == 0);

    // Bit 27 counts one page of the largest kind, which is one base page.
    REQUIRE(DecodeRsc7PageFlags(1U << 27) == kBasePage);

    // Bit 4 counts one page of the smallest kind, 1/16 of the largest, so 256 base pages.
    REQUIRE(DecodeRsc7PageFlags(1U << 4) == 256 * kBasePage);

    // Three pages in the 7-bit field at bit 17, each worth 16 base pages.
    REQUIRE(DecodeRsc7PageFlags(3U << 17) == 48 * kBasePage);

    // The low four bits scale every page: one page, base size shifted left 15 times.
    REQUIRE(DecodeRsc7PageFlags((1U << 27) | 0xF) == (kBasePage << 15));

    // Two fields at once, at scale 1: 5 * 16 + 2 * 64 = 208 pages of 1024 bytes.
    REQUIRE(DecodeRsc7PageFlags((5U << 17) | (2U << 7) | 1U) == 208 * (kBasePage << 1));

    // Every field saturated at the largest scale: 5663 pages of 16 MiB, which no longer fits
    // in 32 bits, and is why the result is 64-bit.
    const uint32_t saturated = (1U << 27) | (1U << 26) | (1U << 25) | (1U << 24) | (0x7FU << 17) |
                               (0x3FU << 11) | (0xFU << 7) | (0x3U << 5) | (1U << 4) | 0xFU;
    REQUIRE(DecodeRsc7PageFlags(saturated) == 5663ULL * (kBasePage << 15));
}

TEST_CASE("RscHeader: reads an RSC7 header", "[streaming]")
{
    TempDir dir;
    dir.WriteFile("asset.ydr", RscHeaderBytes(165, 1U << 27, 1U << 4));

    const std::optional<RscHeader> header = ReadRscHeader(dir.Path() / "asset.ydr");

    REQUIRE(header.has_value());
    REQUIRE(header->format == RscHeader::Format::Rsc7);
    REQUIRE(header->IsResource());
    REQUIRE(header->version == 165);
    REQUIRE(header->VirtualSizeBytes() == 512);
    REQUIRE(header->PhysicalSizeBytes() == 256 * 512);
}

TEST_CASE("RscHeader: recognizes RSC8 and RSC5 as well", "[streaming]")
{
    TempDir dir;
    dir.WriteFile("eight.ydr", RscHeaderBytes(165, 0, 0, 0x38435352));
    dir.WriteFile("five.ydr", RscHeaderBytes(165, 0, 0, 0x05435352));

    REQUIRE(ReadRscHeader(dir.Path() / "eight.ydr")->format == RscHeader::Format::Rsc8);
    REQUIRE(ReadRscHeader(dir.Path() / "five.ydr")->format == RscHeader::Format::Rsc5);
}

TEST_CASE("RscHeader: a file with no magic is readable but not a resource", "[streaming]")
{
    TempDir dir;
    dir.WriteFile("xml.ydr", "<Drawable>uncompiled OpenFormats export</Drawable>");

    const std::optional<RscHeader> header = ReadRscHeader(dir.Path() / "xml.ydr");

    REQUIRE(header.has_value());
    REQUIRE_FALSE(header->IsResource());
    REQUIRE(header->format == RscHeader::Format::None);
    REQUIRE(header->VirtualSizeBytes() == 0);
}

TEST_CASE("RscHeader: a PSO file is recognized, but is not a resource", "[streaming]")
{
    TempDir dir;
    dir.WriteFile("_manifest.ymf", std::string{"PSIN\0\0\0\x10", 8} + std::string(8, '\x7F'));

    const std::optional<RscHeader> header = ReadRscHeader(dir.Path() / "_manifest.ymf");

    REQUIRE(header.has_value());
    REQUIRE(header->IsPsoMetadata());
    REQUIRE_FALSE(header->IsResource());
    REQUIRE(header->VirtualSizeBytes() == 0);
    REQUIRE(spl::streaming::ToString(header->format) == "PSIN");
}

TEST_CASE("RscHeader: a file shorter than a header is not a resource", "[streaming]")
{
    TempDir dir;
    dir.WriteFile("tiny.ydr", "RSC7");

    const std::optional<RscHeader> header = ReadRscHeader(dir.Path() / "tiny.ydr");

    REQUIRE(header.has_value());
    REQUIRE_FALSE(header->IsResource());
}

TEST_CASE("RscHeader: a missing file is a failure, with a reason", "[streaming]")
{
    TempDir dir;
    std::string error;

    const std::optional<RscHeader> header = ReadRscHeader(dir.Path() / "absent.ydr", &error);

    REQUIRE_FALSE(header.has_value());
    REQUIRE(error.find("absent.ydr") != std::string::npos);
}

TEST_CASE("RscHeader: ToString names the format", "[streaming]")
{
    REQUIRE(spl::streaming::ToString(RscHeader::Format::Rsc7) == "RSC7");
    REQUIRE(spl::streaming::ToString(RscHeader::Format::None) == "none");
}
