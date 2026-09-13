#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "core/Result.h"
#include "mods/ModCatalog.h"
#include "mods/ModFileTree.h"
#include "rage/ModArchiveDevice.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"
#include "rpf/RpfReader.h"
#include "streaming/RscHeader.h"
#include "tests/TempTree.h"
#include "tests/rpf/RpfBuilder.h"
#include "util/Glob.h"

using spl::mods::ArchiveEntry;
using spl::mods::ModFile;
using spl::mods::ModFileTree;
using spl::rage::ModArchiveDevice;
using spl::rpf::RpfReader;
using spl::tests::RpfBuilder;
using spl::tests::TempDir;

namespace
{
constexpr uint64_t kFileTime = 133000000000000000ULL;
constexpr uint32_t kVirtualFlags = 0x00040000;
constexpr uint32_t kPhysicalFlags = 0xD108000C;

/// A tree over one archive holding a small resource, a large one, a deflated meta, and a
/// generated manifest, the shapes a mod's layout is made of.
struct Fixture
{
    TempDir dir;
    std::string largeStored;
    std::string smallResource;
    std::unique_ptr<ModFileTree> tree;

    Fixture()
    {
        smallResource = std::string{"RSC7"} + std::string{"\x0D\x00\x00\x00", 4} +
                        std::string{"\x00\x00\x04\x00", 4} + std::string{"\x0C\x00\x08\xD1", 4} +
                        "small-payload";
        RpfBuilder builder;
        builder.AddFile("content/car.ytd", smallResource);
        largeStored =
            builder.AddLargeResource("content/big.ytd", kVirtualFlags, kPhysicalFlags, "payload");
        builder.AddCompressedFile("content/handling.meta", "<handling/>");
        dir.WriteFile("car.rpf", builder.Build());

        std::vector<std::unique_ptr<RpfReader>> archives;
        archives.push_back(std::make_unique<RpfReader>(
            std::move(RpfReader::Open(dir.Path() / "car.rpf").GetValue())));
        const RpfReader* const archive = archives.front().get();
        std::vector<ModFile> files{
            ModFile{.path = "stream/car.ytd", .source = ArchiveEntry{archive, "content/car.ytd"}},
            ModFile{.path = "stream/Big.ytd", .source = ArchiveEntry{archive, "content/big.ytd"}},
            ModFile{.path = "common/data/handling.meta",
                    .source = ArchiveEntry{archive, "content/handling.meta"}},
            ModFile{.path = "fxmanifest.lua", .source = std::string{"game 'gta5'\n"}}};
        tree =
            std::make_unique<ModFileTree>(Root(), std::move(archives), std::move(files), kFileTime);
    }

    [[nodiscard]] std::filesystem::path Root() const
    {
        return dir.Path() / "mods" / "car";
    }
};
} // namespace

TEST_CASE("ModFileTree: the loader reads files as they would be on disk", "[mods]")
{
    const Fixture fixture;
    const ModFileTree& tree = *fixture.tree;

    CHECK(tree.Read(fixture.Root() / "fxmanifest.lua").GetValue() == "game 'gta5'\n");
    CHECK(tree.Read(fixture.Root() / "common/data/handling.meta").GetValue() == "<handling/>");
    CHECK(tree.Read(fixture.Root() / "STREAM/car.ytd").GetValue() == fixture.smallResource);

    // A large resource reads back with its RSC7 header, so the scanner validates it like a file.
    const std::optional<spl::streaming::RscHeader> header =
        spl::streaming::ReadRscHeader(tree, fixture.Root() / "stream/big.ytd");
    REQUIRE(header);
    CHECK(header->format == spl::streaming::RscHeader::Format::Rsc7);
    CHECK(header->physicalFlags == kPhysicalFlags);

    CHECK(tree.Read(fixture.Root() / "stream/absent.ytd").GetError().code ==
          spl::ErrorCode::NotFound);
    CHECK_FALSE(tree.Read(fixture.dir.Path() / "car.rpf"));
    CHECK_FALSE(std::filesystem::exists(fixture.Root()));
}

TEST_CASE("ModFileTree: stats, lists and globs its virtual folders", "[mods]")
{
    const Fixture fixture;
    const ModFileTree& tree = *fixture.tree;

    const std::optional<spl::util::FileTreeStat> root = tree.Stat(fixture.Root());
    REQUIRE(root);
    CHECK(root->isDirectory);
    CHECK(tree.Stat(fixture.Root() / "common/data")->isDirectory);
    CHECK(tree.Stat(fixture.Root() / "common/data/handling.meta")->sizeBytes == 11);
    CHECK(tree.Stat(fixture.Root() / "stream/big.ytd")->sizeBytes == fixture.largeStored.size());
    CHECK_FALSE(tree.Stat(fixture.Root() / "platform"));

    const spl::Result<std::vector<spl::util::FileTreeEntry>> top = tree.List(fixture.Root());
    REQUIRE(top);
    REQUIRE(top.GetValue().size() == 3);
    CHECK(top.GetValue()[0].path == fixture.Root() / "common");
    CHECK(top.GetValue()[0].stat.isDirectory);
    CHECK(top.GetValue()[1].path == fixture.Root() / "fxmanifest.lua");
    CHECK(top.GetValue()[2].path == fixture.Root() / "stream");
    CHECK_FALSE(tree.List(fixture.Root() / "absent"));

    CHECK(spl::util::GlobFiles(tree, fixture.Root(), "stream/*.ytd") ==
          std::vector<std::string>{"stream/Big.ytd", "stream/car.ytd"});
    CHECK(spl::util::GlobFiles(tree, fixture.Root(), "**/*.meta") ==
          std::vector<std::string>{"common/data/handling.meta"});
}

TEST_CASE("ModFileTree: the game gets files as the archive stores them", "[mods]")
{
    const Fixture fixture;
    const ModFileTree& tree = *fixture.tree;

    const ModFile* const big = tree.FindFile("stream/BIG.ytd");
    REQUIRE(big != nullptr);
    const std::optional<spl::rage::DeviceFileInfo> bigInfo = tree.DescribeForGame(*big);
    REQUIRE(bigInfo);
    CHECK(bigInfo->lengthBytes == ModArchiveDevice::kLargeSizeMarker);
    CHECK(bigInfo->fileTime == kFileTime);
    REQUIRE(bigInfo->resource);
    CHECK(bigInfo->resource->physicalFlags == kPhysicalFlags);
    CHECK(bigInfo->resource->version == 13);
    const std::optional<spl::rage::DeviceFileBytes> bigBytes = tree.OpenForGame(*big);
    REQUIRE(bigBytes);
    CHECK(std::string{bigBytes->bytes.data(), bigBytes->bytes.size()} == fixture.largeStored);

    const std::optional<spl::rage::DeviceFileInfo> small =
        tree.DescribeForGame(*tree.FindFile("stream/car.ytd"));
    REQUIRE(small);
    CHECK(small->lengthBytes == fixture.smallResource.size());
    REQUIRE(small->resource);
    CHECK(small->resource->version == 13);
    CHECK(small->resource->virtualFlags == kVirtualFlags);

    const ModFile* const meta = tree.FindFile("common/data/handling.meta");
    CHECK_FALSE(tree.DescribeForGame(*meta)->resource);
    const std::optional<spl::rage::DeviceFileBytes> metaBytes = tree.OpenForGame(*meta);
    REQUIRE(metaBytes);
    CHECK(metaBytes->owned != nullptr);
    CHECK(std::string{metaBytes->bytes.data(), metaBytes->bytes.size()} == "<handling/>");

    const std::vector<spl::rage::DeviceDirectoryEntry> stream = tree.ListForGame("stream");
    REQUIRE(stream.size() == 2);
    CHECK(stream[0].name == "Big.ytd");
    CHECK(stream[0].lengthBytes == ModArchiveDevice::kLargeSizeMarker);
    CHECK(tree.IsDirectory(""));
    CHECK(tree.IsDirectory("Common/Data/"));
    CHECK_FALSE(tree.IsDirectory("stream/car.ytd"));
}

TEST_CASE("ModFileTree: a path given twice keeps its first spelling and last source", "[mods]")
{
    const ModFileTree tree{"C:/mods/x",
                           {},
                           {ModFile{.path = "stream\\A.ytd", .source = std::string{"first"}},
                            ModFile{.path = "stream/a.ytd", .source = std::string{"second"}}},
                           0};

    REQUIRE(tree.GetFiles().size() == 1);
    CHECK(tree.GetFiles()[0].path == "stream/A.ytd");
    CHECK(tree.Read("C:/mods/x/stream/a.ytd").GetValue() == "second");
    CHECK(tree.DescribeForGame(tree.GetFiles()[0])->fileTime != 0); // 0 means "none" to the game
}

TEST_CASE("ModCatalog: a device over the catalog serves every mod by its folder", "[mods]")
{
    auto fixture = std::make_unique<Fixture>();
    spl::mods::ModCatalog catalog;
    catalog.Add(std::shared_ptr<const ModFileTree>{std::move(fixture->tree)});

    ModArchiveDevice root{catalog, ""};
    ModArchiveDevice overlay{catalog, "car/common"};
    void* const rootObject = root.GetGameDevice();
    void* const overlayObject = overlay.GetGameDevice();
    using namespace spl::rage::FileDeviceLayout;
    using OpenFn = uint64_t (*)(void*, const char*, bool);
    using LengthFn = uint64_t (*)(void*, const char*);
    using AttributesFn = uint32_t (*)(void*, const char*);
    const auto slot = [](void* object, std::size_t index)
    { return spl::rage::GetVirtualFunction<void*>(reinterpret_cast<uintptr_t>(object), index); };

    CHECK(reinterpret_cast<LengthFn>(slot(rootObject, kSlotGetFileLengthLong))(
              rootObject, "splmods:/CAR/stream/big.ytd") == ModArchiveDevice::kLargeSizeMarker);
    CHECK(reinterpret_cast<AttributesFn>(slot(rootObject, kSlotGetFileAttributes))(
              rootObject, "splmods:/") == 0x10);
    CHECK(reinterpret_cast<AttributesFn>(slot(rootObject, kSlotGetFileAttributes))(
              rootObject, "splmods:/other/stream/big.ytd") == spl::rage::kInvalidFileAttributesRaw);

    const uint64_t handle = reinterpret_cast<OpenFn>(slot(overlayObject, kSlotOpen))(
        overlayObject, "common:/data/handling.meta", true);
    REQUIRE(handle != spl::rage::kInvalidFileHandleRaw);
    CHECK(catalog.List("").size() == 1);
    CHECK(catalog.List("")[0].name == "car");
    CHECK(catalog.IsDirectory("car/common/data"));
    CHECK(catalog.OpenFile("car/fxmanifest.lua").has_value());
}
