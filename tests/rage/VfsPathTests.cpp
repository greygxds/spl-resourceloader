#include <filesystem>
#include <optional>
#include <string>

#include <catch_amalgamated.hpp>

#include "rage/VfsPath.h"

using namespace spl;

namespace
{
const std::filesystem::path kRoot = "D:/Games/GTAV/resourceLoader/resources";
}

TEST_CASE("VfsPath: the device root ends in exactly one forward slash", "[rage]")
{
    CHECK(rage::MakeDeviceRoot(kRoot) == "D:/Games/GTAV/resourceLoader/resources/");
    CHECK(rage::MakeDeviceRoot("D:/Games/GTAV/resourceLoader/resources/") ==
          "D:/Games/GTAV/resourceLoader/resources/");
    CHECK(rage::MakeDeviceRoot(R"(D:\Games\GTAV\resources)") == "D:/Games/GTAV/resources/");
}

TEST_CASE("VfsPath: a file under the root becomes a splres: path", "[rage]")
{
    const std::optional<std::string> path =
        rage::MakeVfsPath(kRoot, kRoot / "map_one/stream/sub/spl_test.ytd");
    REQUIRE(path);
    CHECK(*path == "splres:/map_one/stream/sub/spl_test.ytd");
}

TEST_CASE("VfsPath: category folders keep their brackets and spaces", "[rage]")
{
    const std::optional<std::string> path =
        rage::MakeVfsPath(kRoot, kRoot / "[maps]/my map/stream/spl_test.ytd");
    REQUIRE(path);
    CHECK(*path == "splres:/[maps]/my map/stream/spl_test.ytd");
}

TEST_CASE("VfsPath: the case on disk is preserved", "[rage]")
{
    const std::optional<std::string> path =
        rage::MakeVfsPath(kRoot, kRoot / "MapOne/stream/SPL_Test.ytd");
    REQUIRE(path);
    CHECK(*path == "splres:/MapOne/stream/SPL_Test.ytd");
}

TEST_CASE("VfsPath: a trailing slash on the root changes nothing", "[rage]")
{
    const std::optional<std::string> path =
        rage::MakeVfsPath("D:/Games/GTAV/resources/", "D:/Games/GTAV/resources/a/b.ytd");
    REQUIRE(path);
    CHECK(*path == "splres:/a/b.ytd");
}

TEST_CASE("VfsPath: a file outside the root has no VFS path", "[rage]")
{
    CHECK_FALSE(rage::MakeVfsPath(kRoot, "D:/Games/GTAV/update/x1.rpf"));
    CHECK_FALSE(rage::MakeVfsPath(kRoot, "E:/elsewhere/spl_test.ytd"));
    CHECK_FALSE(rage::MakeVfsPath(kRoot, kRoot));
}

TEST_CASE("VfsPath: the mods mount names cache files", "[rage]")
{
    const std::filesystem::path cache = "D:/Games/GTAV/resourceLoader/mods_cache";
    const std::optional<std::string> path = rage::MakeVfsPath(
        cache, rage::kModsMountPoint, cache / "cycochy/stream/mp_freemode_01.ytd");
    REQUIRE(path);
    CHECK(*path == "splmods:/cycochy/stream/mp_freemode_01.ytd");
    CHECK_FALSE(rage::MakeVfsPath(cache, rage::kModsMountPoint, kRoot / "a.ytd"));
}

TEST_CASE("VfsPath: non-ASCII roots are recognized so the caller can warn", "[rage]")
{
    CHECK(rage::IsPrintableAscii("D:/Games/GTAV/resources/"));
    CHECK(rage::IsPrintableAscii("[maps]/my map/"));
    CHECK_FALSE(rage::IsPrintableAscii("D:/Jeux/r\xc3\xa9sources/"));
}

TEST_CASE("VfsPath: a data-file entry keeps a VFS path that fits", "[rage]")
{
    const std::string path = "splres:/props/stream/spl_props.ytyp";
    using spl::rage::DataFileNaming;
    CHECK(spl::rage::MakeDataFileEntryName(path, DataFileNaming::PathOnly) == path);
    CHECK(spl::rage::MakeDataFileEntryName(std::string(127, 'a'), DataFileNaming::PathOnly) ==
          std::string(127, 'a'));
    const std::string vehicle = "splres:/sg720sgt3/data/Mclaren 720S GT3/handling.meta";
    CHECK(spl::rage::MakeDataFileEntryName(vehicle, DataFileNaming::PathOnly) == vehicle);
}

TEST_CASE("VfsPath: a ytyp entry that does not fit is shortened to its file name", "[rage]")
{
    const std::string path = "splres:/" + std::string(130, 'x') + "/stream/spl_props.ytyp";
    CHECK(spl::rage::MakeDataFileEntryName(path, spl::rage::DataFileNaming::BaseNameIsEnough) ==
          "spl/spl_props.ytyp");
}

TEST_CASE("VfsPath: any other data-file entry that does not fit is refused", "[rage]")
{
    const std::string path = "splres:/" + std::string(130, 'x') + "/data/handling.meta";
    CHECK_FALSE(
        spl::rage::MakeDataFileEntryName(path, spl::rage::DataFileNaming::PathOnly).has_value());
}

TEST_CASE("VfsPath: a data-file entry with a file name too long for the game is refused", "[rage]")
{
    const std::string path = "splres:/props/" + std::string(125, 'y') + ".ytyp";
    CHECK_FALSE(spl::rage::MakeDataFileEntryName(path, spl::rage::DataFileNaming::BaseNameIsEnough)
                    .has_value());
}
