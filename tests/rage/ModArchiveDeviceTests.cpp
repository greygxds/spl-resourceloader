#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rage/ModArchiveDevice.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"

using namespace spl::rage;
using namespace spl::rage::FileDeviceLayout;

namespace
{
constexpr uint64_t kFileTime = 133000000000000000ULL;

/// Files by lower-case source path; folders are implied by the paths.
class FakeSource final : public IDeviceFileSource
{
public:
    std::map<std::string, std::string> files;
    std::map<std::string, DeviceFileInfo> infos;

    void Add(std::string path, std::string contents,
             std::optional<DeviceFileInfo::ResourceVersion> resource = std::nullopt,
             std::optional<uint64_t> lengthBytes = std::nullopt)
    {
        infos[path] = DeviceFileInfo{.lengthBytes = lengthBytes.value_or(contents.size()),
                                     .fileTime = kFileTime,
                                     .resource = resource};
        files[std::move(path)] = std::move(contents);
    }

    [[nodiscard]] std::optional<DeviceFileInfo> FindFile(std::string_view path) const override
    {
        const auto found = infos.find(std::string{path});
        return found != infos.end() ? std::optional{found->second} : std::nullopt;
    }

    [[nodiscard]] bool IsDirectory(std::string_view path) const override
    {
        const std::string prefix = path.empty() ? std::string{} : std::string{path} + "/";
        for (const auto& [name, contents] : files)
        {
            if (name.starts_with(prefix) && name != path)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<DeviceFileBytes> OpenFile(std::string_view path) const override
    {
        const auto found = files.find(std::string{path});
        if (found == files.end())
        {
            return std::nullopt;
        }
        return DeviceFileBytes{.bytes = std::span<const char>{found->second}};
    }

    [[nodiscard]] std::vector<DeviceDirectoryEntry> List(std::string_view path) const override
    {
        std::vector<DeviceDirectoryEntry> entries;
        const std::string prefix = path.empty() ? std::string{} : std::string{path} + "/";
        for (const auto& [name, contents] : files)
        {
            if (!name.starts_with(prefix) || name == path)
            {
                continue;
            }
            const std::string rest = name.substr(prefix.size());
            const std::size_t slash = rest.find('/');
            const std::string child = rest.substr(0, slash);
            if (!entries.empty() && entries.back().name == child)
            {
                continue;
            }
            entries.push_back(DeviceDirectoryEntry{.name = child,
                                                   .isDirectory = slash != std::string::npos,
                                                   .lengthBytes = contents.size(),
                                                   .fileTime = kFileTime});
        }
        return entries;
    }
};

/// A call through the device's vtable, exactly the way game code makes it.
template <typename TFn, typename... TArgs>
auto CallSlot(ModArchiveDevice& device, std::size_t slot, TArgs&&... arguments)
{
    void* const object = device.GetGameDevice();
    const auto function = GetVirtualFunction<TFn>(reinterpret_cast<uintptr_t>(object), slot);
    return function(object, std::forward<TArgs>(arguments)...);
}

using OpenFn = uint64_t (*)(void*, const char*, bool);
using OpenBulkFn = uint64_t (*)(void*, const char*, uint64_t*);
using ReadFn = uint32_t (*)(void*, uint64_t, void*, uint32_t);
using ReadBulkFn = uint32_t (*)(void*, uint64_t, uint64_t, void*, uint32_t);
using SeekLongFn = uint64_t (*)(void*, uint64_t, int64_t, uint32_t);
using HandleFn = uint64_t (*)(void*, uint64_t);
using CloseFn = int32_t (*)(void*, uint64_t);
using PathFn = uint64_t (*)(void*, const char*);
using AttributesFn = uint32_t (*)(void*, const char*);
using FindFirstFn = uint64_t (*)(void*, const char*, FindDataView*);
using FindNextFn = bool (*)(void*, uint64_t, FindDataView*);
using ResourceVersionFn = int32_t (*)(void*, const char*, ResourceFlagsView*);
using NameFn = const char* (*)(void*);
using NoArgumentFn = uint64_t (*)(void*);

[[nodiscard]] std::string ReadAll(ModArchiveDevice& device, uint64_t handle)
{
    std::string contents;
    std::array<char, 3> chunk{};
    while (const uint32_t count =
               CallSlot<ReadFn>(device, kSlotRead, handle, chunk.data(), uint32_t{3}))
    {
        contents.append(chunk.data(), count);
    }
    return contents;
}
} // namespace

TEST_CASE("ModArchiveDevice: maps game paths into its folder", "[rage]")
{
    FakeSource source;
    ModArchiveDevice root{source, ""};
    ModArchiveDevice overlay{source, "MyCar\\common/"};

    CHECK(root.ToSourcePath("splmods:/MyCar/stream//car.ytd") == "mycar/stream/car.ytd");
    CHECK(root.ToSourcePath("splmods:/") == "");
    CHECK(overlay.ToSourcePath("common:/data\\handling.meta") == "mycar/common/data/handling.meta");
    CHECK(overlay.ToSourcePath("commoncrc:/") == "mycar/common");
}

TEST_CASE("ModArchiveDevice: opens, reads, seeks and closes a file", "[rage]")
{
    FakeSource source;
    source.Add("mycar/common/data/handling.meta", "<handling/>");
    ModArchiveDevice device{source, "mycar/common"};

    const uint64_t handle = CallSlot<OpenFn>(device, kSlotOpen, "common:/data/handling.meta", true);
    REQUIRE(handle != kInvalidFileHandleRaw);
    CHECK(CallSlot<HandleFn>(device, kSlotGetFileLength, handle) == 11);
    CHECK(ReadAll(device, handle) == "<handling/>");

    CHECK(CallSlot<SeekLongFn>(device, kSlotSeekLong, handle, int64_t{-3}, uint32_t{2}) == 8);
    CHECK(ReadAll(device, handle) == "g/>");
    CHECK(CallSlot<SeekLongFn>(device, kSlotSeekLong, handle, int64_t{1}, uint32_t{0}) == 1);
    CHECK(CallSlot<SeekLongFn>(device, kSlotSeekLong, handle, int64_t{99}, uint32_t{1}) == 11);

    CHECK(CallSlot<CloseFn>(device, kSlotClose, handle) == 0);
    CHECK(device.CountOpenHandles() == 0);
    CHECK(CallSlot<CloseFn>(device, kSlotClose, handle) == -1);
    std::array<char, 4> buffer{};
    CHECK(CallSlot<ReadFn>(device, kSlotRead, handle, buffer.data(), uint32_t{4}) == UINT32_MAX);

    CHECK(CallSlot<OpenFn>(device, kSlotOpen, "common:/data/absent.meta", true) ==
          kInvalidFileHandleRaw);
    CHECK(CallSlot<OpenFn>(device, kSlotOpen, "common:/data/handling.meta", false) ==
          kInvalidFileHandleRaw); // read-only
}

TEST_CASE("ModArchiveDevice: serves a large resource the way a packfile stores it", "[rage]")
{
    FakeSource source;
    const std::string stored = std::string(16, 'S') + "payload";
    source.Add("mycar/stream/big.ytd", stored,
               DeviceFileInfo::ResourceVersion{
                   .version = 13, .virtualFlags = 0x00040000, .physicalFlags = 0xD108000C},
               ModArchiveDevice::kLargeSizeMarker);
    ModArchiveDevice device{source, ""};

    uint64_t bulkOffset = 99;
    const uint64_t handle =
        CallSlot<OpenBulkFn>(device, kSlotOpenBulk, "splmods:/mycar/stream/big.ytd", &bulkOffset);
    REQUIRE(handle != kInvalidFileHandleRaw);
    CHECK(bulkOffset == 0);
    CHECK(CallSlot<HandleFn>(device, kSlotGetFileLengthUInt64, handle) ==
          ModArchiveDevice::kLargeSizeMarker);
    CHECK(CallSlot<PathFn>(device, kSlotGetFileLengthLong, "splmods:/mycar/stream/big.ytd") ==
          ModArchiveDevice::kLargeSizeMarker);

    std::array<char, 16> header{};
    CHECK(CallSlot<ReadBulkFn>(device, kSlotReadBulk, handle, uint64_t{0}, header.data(),
                               uint32_t{16}) == 16);
    CHECK(std::string{header.data(), header.size()} == std::string(16, 'S'));
    std::array<char, 16> tail{};
    CHECK(CallSlot<ReadBulkFn>(device, kSlotReadBulk, handle, uint64_t{16}, tail.data(),
                               uint32_t{16}) == 7);
    CHECK(std::string{tail.data(), 7} == "payload");

    ResourceFlagsView flags{};
    CHECK(CallSlot<ResourceVersionFn>(device, kSlotGetResourceVersion,
                                      "splmods:/mycar/stream/big.ytd", &flags) == 13);
    CHECK(flags.physicalFlags == 0xD108000C);
    CHECK(CallSlot<CloseFn>(device, kSlotCloseBulk, handle) == 0);
}

TEST_CASE("ModArchiveDevice: answers attributes, times and searches", "[rage]")
{
    FakeSource source;
    source.Add("mycar/common/data/a.meta", "a");
    source.Add("mycar/common/data/b.meta", "bb");
    ModArchiveDevice device{source, "mycar/common"};

    CHECK(CallSlot<AttributesFn>(device, kSlotGetFileAttributes, "common:/data") == 0x10);
    CHECK(CallSlot<AttributesFn>(device, kSlotGetFileAttributes, "common:/data/a.meta") == 0);
    CHECK(CallSlot<AttributesFn>(device, kSlotGetFileAttributes, "common:/data/c.meta") ==
          kInvalidFileAttributesRaw);
    CHECK(CallSlot<PathFn>(device, kSlotGetFileTime, "common:/data/a.meta") == kFileTime);

    FindDataView findData{};
    const uint64_t search =
        CallSlot<FindFirstFn>(device, kSlotFindFirst, "common:/data", &findData);
    REQUIRE(search != kInvalidFileHandleRaw);
    CHECK(std::string_view{findData.fileName} == "a.meta");
    REQUIRE(CallSlot<FindNextFn>(device, kSlotFindNext, search, &findData));
    CHECK(std::string_view{findData.fileName} == "b.meta");
    CHECK(findData.fileSize == 2);
    CHECK_FALSE(CallSlot<FindNextFn>(device, kSlotFindNext, search, &findData));
    CHECK(CallSlot<CloseFn>(device, kSlotFindClose, search) == 0);

    const uint64_t single =
        CallSlot<FindFirstFn>(device, kSlotFindFirst, "common:/data/b.meta", &findData);
    REQUIRE(single != kInvalidFileHandleRaw);
    CHECK(std::string_view{findData.fileName} == "b.meta");
    CHECK(CallSlot<CloseFn>(device, kSlotFindClose, single) == 0);
    CHECK(CallSlot<FindFirstFn>(device, kSlotFindFirst, "common:/none", &findData) ==
          kInvalidFileHandleRaw);

    ResourceFlagsView flags{.virtualFlags = 1, .physicalFlags = 1};
    CHECK(CallSlot<ResourceVersionFn>(device, kSlotGetResourceVersion, "common:/data/a.meta",
                                      &flags) == 0);
    CHECK(flags.virtualFlags == 0);
    CHECK(device.CountOpenHandles() == 0);
}

TEST_CASE("ModArchiveDevice: refuses writes and records slots it does not know", "[rage]")
{
    FakeSource source;
    source.Add("x.meta", "x");
    ModArchiveDevice device{source, ""};

    CHECK(CallSlot<PathFn>(device, kSlotCreate, "splmods:/y.meta") == kInvalidFileHandleRaw);
    CHECK_FALSE(
        static_cast<bool>(CallSlot<PathFn>(device, kSlotRemoveFile, "splmods:/x.meta") & 0xFF));
    CHECK(std::string_view{CallSlot<NameFn>(device, kSlotGetName)} == ModArchiveDevice::kName);
    CHECK((CallSlot<NoArgumentFn>(device, kSlotIsCollection) & 0xFF) == 0);
    CHECK(device.GetUnexpectedSlots().empty());

    constexpr std::size_t kUnknownSlot = 28; // FiveM's GetUnkDevice
    CHECK(CallSlot<NoArgumentFn>(device, kUnknownSlot) == 0);
    CHECK(CallSlot<NoArgumentFn>(device, kUnknownSlot) == 0);
    CHECK(device.GetUnexpectedSlots() == std::vector<std::size_t>{kUnknownSlot});
}

TEST_CASE("ModArchiveDevice: concurrent readers keep their own cursors", "[rage]")
{
    FakeSource source;
    source.Add("big.bin", std::string(4096, 'z'));
    ModArchiveDevice device{source, ""};

    std::vector<std::jthread> readers;
    std::array<std::size_t, 8> totals{};
    for (std::size_t reader = 0; reader < totals.size(); ++reader)
    {
        readers.emplace_back(
            [&device, &totals, reader]
            {
                for (int round = 0; round < 50; ++round)
                {
                    const uint64_t handle =
                        CallSlot<OpenFn>(device, kSlotOpen, "splmods:/big.bin", true);
                    totals[reader] += ReadAll(device, handle).size();
                    (void)CallSlot<CloseFn>(device, kSlotClose, handle);
                }
            });
    }
    readers.clear();
    for (const std::size_t total : totals)
    {
        CHECK(total == 4096 * 50);
    }
    CHECK(device.CountOpenHandles() == 0);
}
