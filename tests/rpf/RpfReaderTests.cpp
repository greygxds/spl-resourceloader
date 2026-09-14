#include <span>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "core/Result.h"
#include "rpf/RpfReader.h"
#include "tests/TempTree.h"
#include "tests/rpf/RpfBuilder.h"

using spl::ErrorCode;
using spl::rpf::RpfReader;
using spl::tests::RpfBuilder;
using spl::tests::TempDir;

namespace
{
/// Writes bytes as "mod.rpf" and opens it, the common arrange step.
[[nodiscard]] spl::Result<RpfReader> OpenBytes(const TempDir& dir, const std::string& bytes)
{
    dir.WriteFile("mod.rpf", bytes);
    return RpfReader::Open(dir.Path() / "mod.rpf");
}

[[nodiscard]] bool HasPath(const std::vector<RpfReader::EntryInfo>& entries, std::string_view path,
                           bool isDirectory)
{
    for (const RpfReader::EntryInfo& entry : entries)
    {
        if (entry.path == path && entry.isDirectory == isDirectory)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string BytesToString(const std::vector<std::byte>& bytes)
{
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte byte : bytes)
    {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}
} // namespace

TEST_CASE("RpfReader: opens a valid archive and enumerates it", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/car.ytd", "texture-bytes");
    builder.AddFile("content/sub/nested.yft", "model-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());

    const RpfReader& reader = opened.GetValue();
    REQUIRE(reader.GetEntryCount() ==
            6); // root + assembly.xml + content + car.ytd + sub + nested.yft

    const std::vector<RpfReader::EntryInfo> entries = reader.Enumerate();
    REQUIRE(entries.size() == 5);
    CHECK(HasPath(entries, "assembly.xml", false));
    CHECK(HasPath(entries, "content", true));
    CHECK(HasPath(entries, "content/car.ytd", false));
    CHECK(HasPath(entries, "content/sub", true));
    CHECK(HasPath(entries, "content/sub/nested.yft", false));

    for (const RpfReader::EntryInfo& entry : entries)
    {
        if (entry.path == "content/car.ytd")
        {
            CHECK(entry.sizeBytes == 13);
        }
        if (entry.isDirectory)
        {
            CHECK(entry.sizeBytes == 0);
        }
    }
}

TEST_CASE("RpfReader: enumerates an empty archive", "[rpf]")
{
    TempDir dir;
    const spl::Result<RpfReader> opened = OpenBytes(dir, RpfBuilder{}.Build());

    REQUIRE(opened.HasValue());
    CHECK(opened.GetValue().GetEntryCount() == 1);
    CHECK(opened.GetValue().Enumerate().empty());
}

TEST_CASE("RpfReader: rejects files that are not RPF7", "[rpf]")
{
    TempDir dir;
    dir.WriteFile("mod.rpf", "definitely not an archive");

    const spl::Result<RpfReader> opened = RpfReader::Open(dir.Path() / "mod.rpf");
    REQUIRE(!opened.HasValue());
    CHECK(opened.GetError().code == ErrorCode::NotSupported);
}

TEST_CASE("RpfReader: opens signed archives without checking the signature", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    std::string bytes = builder.Build();
    bytes[12] = 'C'; // "OPEN" -> "CFXP": the encryption word at +0x0C
    bytes[13] = 'F';
    bytes[14] = 'X';
    bytes[15] = 'P';

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(opened.HasValue());
    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("assembly.xml");
    REQUIRE(found.HasValue());
    CHECK(BytesToString(found.GetValue()) == "<package/>");
}

TEST_CASE("RpfReader: rejects encrypted archives", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    std::string bytes = builder.Build();
    bytes[12] = 'N'; // any other encryption word is AES or NG
    bytes[13] = 'G';
    bytes[14] = '@';
    bytes[15] = '@';

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(!opened.HasValue());
    CHECK(opened.GetError().code == ErrorCode::NotSupported);
}

TEST_CASE("RpfReader: rejects truncated files", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("content/car.ytd", "texture-bytes");
    const std::string full = builder.Build();

    // The last cut lands inside the file's only payload (past the padding the
    // builder appends, which the format does not require).
    const std::vector<std::size_t> cutPoints{0, 7, 16, 48, full.size() - 513};
    for (const std::size_t size : cutPoints)
    {
        const spl::Result<RpfReader> opened = OpenBytes(dir, full.substr(0, size));
        REQUIRE(!opened.HasValue());
        CHECK(opened.GetError().code == ErrorCode::Parse);
    }
}

TEST_CASE("RpfReader: reads stored files", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("assembly.xml", "<package/>");
    builder.AddFile("content/car.ytd", "texture-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());

    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("content/car.ytd");
    REQUIRE(found.HasValue());
    CHECK(BytesToString(found.GetValue()) == "texture-bytes");
}

TEST_CASE("RpfReader: lookup ignores leading slashes and case", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("content/car.ytd", "texture-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());
    const RpfReader& reader = opened.GetValue();

    const spl::Result<std::vector<std::byte>> slashed = reader.ReadFile("/content/car.ytd");
    REQUIRE(slashed.HasValue());
    CHECK(BytesToString(slashed.GetValue()) == "texture-bytes");

    const spl::Result<std::vector<std::byte>> folded = reader.ReadFile("CONTENT/CAR.YTD");
    REQUIRE(folded.HasValue());
    CHECK(BytesToString(folded.GetValue()) == "texture-bytes");
}

TEST_CASE("RpfReader: reading a missing path or a directory fails", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("content/car.ytd", "texture-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());
    const RpfReader& reader = opened.GetValue();

    const spl::Result<std::vector<std::byte>> missing = reader.ReadFile("content/nope.ytd");
    REQUIRE(!missing.HasValue());
    CHECK(missing.GetError().code == ErrorCode::NotFound);

    const spl::Result<std::vector<std::byte>> directory = reader.ReadFile("content");
    REQUIRE(!directory.HasValue());
    CHECK(directory.GetError().code == ErrorCode::InvalidArgument);
}

TEST_CASE("RpfReader: inflates compressed entries", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddCompressedFile("content/car.ytd", "texture-bytes-texture-bytes-texture-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());

    const std::vector<RpfReader::EntryInfo> entries = opened.GetValue().Enumerate();
    REQUIRE(entries.size() == 2);
    CHECK(entries[1].sizeBytes == 41);

    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("content/car.ytd");
    REQUIRE(found.HasValue());
    CHECK(BytesToString(found.GetValue()) == "texture-bytes-texture-bytes-texture-bytes");
}

TEST_CASE("RpfReader: corrupt compressed data fails to inflate", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddCompressedFile("content/car.ytd", "texture-bytes-texture-bytes-texture-bytes");
    std::string bytes = builder.Build();
    // Flip a byte inside the deflated payload: the builder pads each payload to a sector,
    // so the payload starts one sector before the end of this two-sector file.
    const std::size_t payload = bytes.size() - 512 + 2;
    bytes[payload] = static_cast<char>(bytes[payload] ^ 0xFF);

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(opened.HasValue());

    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("content/car.ytd");
    REQUIRE(!found.HasValue());
    CHECK(found.GetError().code == ErrorCode::Parse);
}

TEST_CASE("RpfReader: lookup normalizes backslashes", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("content/sub/car.ytd", "texture-bytes");

    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());

    // Mod assemblies spell sources with backslashes; the archive hierarchy does not.
    const spl::Result<std::vector<std::byte>> found =
        opened.GetValue().ReadFile("content\\sub\\car.ytd");
    REQUIRE(found.HasValue());
    CHECK(BytesToString(found.GetValue()) == "texture-bytes");
}

TEST_CASE("RpfReader: reads size-zero stored entries", "[rpf]")
{
    // The shape FiveM's own tools write: stored with an empty size field, so the length
    // comes from virtFlags. Real OpenIV packages use sized stored entries instead.
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("a.ytd", "texture-bytes");
    std::string bytes = builder.Build();
    bytes[16 + 16 + 2] = '\0';
    bytes[16 + 16 + 3] = '\0';
    bytes[16 + 16 + 4] = '\0';
    bytes[16 + 16 + 7] = static_cast<char>(bytes[16 + 16 + 7] & ~0x80);

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(opened.HasValue());

    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("a.ytd");
    REQUIRE(found.HasValue());
    CHECK(BytesToString(found.GetValue()) == "texture-bytes");
}

TEST_CASE("RpfReader: rejects a missing file", "[rpf]")
{
    const spl::Result<RpfReader> opened = RpfReader::Open("does-not-exist.rpf");
    REQUIRE(!opened.HasValue());
    CHECK(opened.GetError().code == ErrorCode::Io);
}

TEST_CASE("RpfReader: rejects child ranges outside the table", "[rpf]")
{
    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("content/car.ytd", "texture-bytes");
    std::string bytes = builder.Build();
    // Root is entry 0: its child count lives at +16+12. Inflate it past the table.
    bytes[16 + 12] = static_cast<char>(0xFF);

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(!opened.HasValue());
    CHECK(opened.GetError().code == ErrorCode::Parse);
}

TEST_CASE("RpfReader: a resource too large for the size field gets its RSC7 header back", "[rpf]")
{
    // Resources over 16 MiB set the size field to 0xFFFFFF and keep the real size in their first
    // 16 bytes, where the RSC7 header would be (CodeWalker RpfResourceFileEntry).
    const std::string payload = "compressed-resource-payload";
    const auto total = static_cast<uint32_t>(16 + payload.size());
    std::string sizeHeader(16, '\0');
    sizeHeader[7] = static_cast<char>(total & 0xFF);
    sizeHeader[14] = static_cast<char>((total >> 8) & 0xFF);
    sizeHeader[5] = static_cast<char>((total >> 16) & 0xFF);
    sizeHeader[2] = static_cast<char>((total >> 24) & 0xFF);

    TempDir dir;
    RpfBuilder builder;
    builder.AddFile("big.ytd", sizeHeader + payload);
    std::string bytes = builder.Build();
    // Entry 1 is the file: the size field sits at +2..+4 of its packed word; virt and phys follow.
    const std::size_t entry = 16 + 16;
    bytes[entry + 2] = static_cast<char>(0xFF);
    bytes[entry + 3] = static_cast<char>(0xFF);
    bytes[entry + 4] = static_cast<char>(0xFF);
    const auto putU32 = [&bytes](std::size_t at, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes[at + shift / 8] = static_cast<char>((value >> shift) & 0xFF);
        }
    };
    putU32(entry + 8, 0x00040000);  // virtual flags: version nibble 0
    putU32(entry + 12, 0xD108000C); // physical flags: version nibble 13, a texture dictionary
    bytes.append(512, '\0');        // room past the declared size, as real archives have

    const spl::Result<RpfReader> opened = OpenBytes(dir, bytes);
    REQUIRE(opened.HasValue());
    const spl::Result<std::vector<std::byte>> found = opened.GetValue().ReadFile("big.ytd");
    REQUIRE(found.HasValue());
    const std::string contents = BytesToString(found.GetValue());

    REQUIRE(contents.size() == total);
    CHECK(contents.substr(0, 4) == "RSC7");
    CHECK(contents[4] == 13);
    CHECK(contents.substr(8, 4) == std::string{"\x00\x00\x04\x00", 4});
    CHECK(contents.substr(12, 4) == std::string{"\x0C\x00\x08\xD1", 4});
    CHECK(contents.substr(16) == payload);
    CHECK(opened.GetValue().Enumerate().front().sizeBytes == total);

    const spl::Result<RpfReader::FileInfo> info = opened.GetValue().Stat("big.ytd");
    REQUIRE(info.HasValue());
    CHECK(info.GetValue().sizeBytes == total);
    CHECK(info.GetValue().isStored);
    CHECK(info.GetValue().isLargeResource);
    CHECK(info.GetValue().physicalFlags == 0xD108000C);

    // The game reads the archive's own bytes: the size header, not the synthesized RSC7.
    const spl::Result<std::span<const char>> stored = opened.GetValue().GetStoredBytes("big.ytd");
    REQUIRE(stored.HasValue());
    CHECK(std::string{stored.GetValue().data(), stored.GetValue().size()} == sizeHeader + payload);

    const spl::Result<std::vector<std::byte>> prefix = opened.GetValue().ReadPrefix("big.ytd", 4);
    REQUIRE(prefix.HasValue());
    CHECK(BytesToString(prefix.GetValue()) == "RSC7");
}

TEST_CASE("RpfReader: stats, prefixes and stored bytes of ordinary files", "[rpf]")
{
    RpfBuilder builder;
    builder.AddFile("stored.meta", "stored contents");
    builder.AddCompressedFile("packed.meta", "packed contents");
    TempDir dir;
    const spl::Result<RpfReader> opened = OpenBytes(dir, builder.Build());
    REQUIRE(opened.HasValue());
    const RpfReader& reader = opened.GetValue();

    const spl::Result<RpfReader::FileInfo> stored = reader.Stat("stored.meta");
    REQUIRE(stored.HasValue());
    CHECK(stored.GetValue().sizeBytes == 15);
    CHECK(stored.GetValue().isStored);
    CHECK_FALSE(stored.GetValue().isLargeResource);
    const spl::Result<std::span<const char>> bytes = reader.GetStoredBytes("stored.meta");
    REQUIRE(bytes.HasValue());
    CHECK(std::string{bytes.GetValue().data(), bytes.GetValue().size()} == "stored contents");

    const spl::Result<RpfReader::FileInfo> packed = reader.Stat("packed.meta");
    REQUIRE(packed.HasValue());
    CHECK(packed.GetValue().sizeBytes == 15);
    CHECK_FALSE(packed.GetValue().isStored);
    CHECK(reader.GetStoredBytes("packed.meta").GetError().code == ErrorCode::NotSupported);

    CHECK(BytesToString(reader.ReadPrefix("stored.meta", 6).GetValue()) == "stored");
    CHECK(BytesToString(reader.ReadPrefix("packed.meta", 6).GetValue()) == "packed");
    CHECK(BytesToString(reader.ReadPrefix("packed.meta", 99).GetValue()) == "packed contents");
    CHECK(reader.Stat("missing.meta").GetError().code == ErrorCode::NotFound);
}

TEST_CASE("RpfReader: opens a stored nested archive in place", "[rpf]")
{
    RpfBuilder inner;
    inner.AddFile("setup2.xml", "<SSetupData/>");
    RpfBuilder outer;
    outer.AddFile("content/dlc.rpf", inner.Build());
    outer.AddCompressedFile("content/packed.rpf", inner.Build());

    TempDir dir;
    const spl::Result<RpfReader> opened = OpenBytes(dir, outer.Build());
    REQUIRE(opened.HasValue());

    for (const std::string_view nested : {"content/dlc.rpf", "content/packed.rpf"})
    {
        const spl::Result<RpfReader> pack = opened.GetValue().OpenNested(nested);
        REQUIRE(pack.HasValue());
        const spl::Result<std::vector<std::byte>> found = pack.GetValue().ReadFile("setup2.xml");
        REQUIRE(found.HasValue());
        CHECK(BytesToString(found.GetValue()) == "<SSetupData/>");
    }
    CHECK(opened.GetValue().OpenNested("content/missing.rpf").GetError().code ==
          ErrorCode::NotFound);
}
