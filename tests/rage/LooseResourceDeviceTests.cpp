#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rage/LooseResourceDevice.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"

using namespace spl::rage;
using namespace spl::rage::FileDeviceLayout;

namespace
{
constexpr std::size_t kFakeSlots = 64;
constexpr uint32_t kLargeBytes = 0x04F50F0E; // the size of a real 80 MiB texture dictionary

struct FakeFile
{
    uint64_t sizeBytes = 0;
    uint32_t magic = 0;
};

/// Stands in for the fiDeviceRelative the device fronts: files by path, handles numbered from 1,
/// every call counted by slot.
struct FakeDevice
{
    const void* const* vtable;
    std::map<std::string, FakeFile> files;
    std::map<uint64_t, std::string> open;
    std::vector<std::size_t> calls;
    uint64_t nextHandle = 1;
};

FakeDevice& Fake(void* self)
{
    return *static_cast<FakeDevice*>(self);
}

template <std::size_t Slot> uint64_t Unexpected(void* self, uint64_t, uint64_t, uint64_t, uint64_t)
{
    Fake(self).calls.push_back(Slot);
    return 0x1000 + Slot;
}

uint64_t FakeOpen(void* self, const char* path, bool /*readOnly*/)
{
    Fake(self).calls.push_back(kSlotOpen);
    if (!Fake(self).files.contains(path))
    {
        return kInvalidFileHandleRaw;
    }
    const uint64_t handle = Fake(self).nextHandle++;
    Fake(self).open[handle] = path;
    return handle;
}

uint64_t FakeOpenBulk(void* self, const char* path, uint64_t* offset)
{
    *offset = 0;
    const uint64_t handle = FakeOpen(self, path, true);
    Fake(self).calls.back() = kSlotOpenBulk;
    return handle;
}

uint32_t FakeRead(void* self, uint64_t handle, void* buffer, uint32_t length)
{
    Fake(self).calls.push_back(kSlotRead);
    const uint32_t magic = Fake(self).files.at(Fake(self).open.at(handle)).magic;
    std::memcpy(buffer, &magic, std::min<uint32_t>(length, sizeof(magic)));
    return std::min<uint32_t>(length, sizeof(magic));
}

uint32_t FakeReadBulk(void* self, uint64_t /*handle*/, uint64_t /*offset*/, void* buffer,
                      uint32_t length)
{
    Fake(self).calls.push_back(kSlotReadBulk);
    std::memset(buffer, 'R', length);
    return length;
}

int32_t FakeClose(void* self, uint64_t handle)
{
    Fake(self).calls.push_back(kSlotClose);
    Fake(self).open.erase(handle);
    return 0;
}

uint64_t FakeLengthOfHandle(void* self, uint64_t handle)
{
    Fake(self).calls.push_back(kSlotGetFileLength);
    return Fake(self).files.at(Fake(self).open.at(handle)).sizeBytes;
}

uint64_t FakeLengthOfPath(void* self, const char* path)
{
    Fake(self).calls.push_back(kSlotGetFileLengthLong);
    const auto found = Fake(self).files.find(path);
    return found != Fake(self).files.end() ? found->second.sizeBytes : 0;
}

const std::array<const void*, kFakeSlots> kFakeVtable = []
{
    std::array<const void*, kFakeSlots> vtable =
        []<std::size_t... Slots>(std::index_sequence<Slots...>)
    {
        return std::array<const void*, kFakeSlots>{
            reinterpret_cast<const void*>(&Unexpected<Slots>)...};
    }(std::make_index_sequence<kFakeSlots>{});
    vtable[kSlotOpen] = reinterpret_cast<const void*>(&FakeOpen);
    vtable[kSlotOpenBulk] = reinterpret_cast<const void*>(&FakeOpenBulk);
    vtable[kSlotRead] = reinterpret_cast<const void*>(&FakeRead);
    vtable[kSlotReadBulk] = reinterpret_cast<const void*>(&FakeReadBulk);
    vtable[kSlotClose] = reinterpret_cast<const void*>(&FakeClose);
    vtable[kSlotCloseBulk] = reinterpret_cast<const void*>(&FakeClose);
    vtable[kSlotGetFileLength] = reinterpret_cast<const void*>(&FakeLengthOfHandle);
    vtable[kSlotGetFileLengthUInt64] = reinterpret_cast<const void*>(&FakeLengthOfHandle);
    vtable[kSlotGetFileLengthLong] = reinterpret_cast<const void*>(&FakeLengthOfPath);
    return vtable;
}();

template <typename TFn, typename... TArgs>
auto CallSlot(LooseResourceDevice& device, std::size_t slot, TArgs&&... arguments)
{
    void* const object = device.GetGameDevice();
    const auto function = GetVirtualFunction<TFn>(reinterpret_cast<uintptr_t>(object), slot);
    return function(object, std::forward<TArgs>(arguments)...);
}

using LengthOfPathFn = uint64_t (*)(void*, const char*);
using LengthOfHandleFn = uint64_t (*)(void*, uint64_t);
using OpenBulkFn = uint64_t (*)(void*, const char*, uint64_t*);
using ReadBulkFn = uint32_t (*)(void*, uint64_t, uint64_t, void*, uint32_t);
using CloseFn = int32_t (*)(void*, uint64_t);

FakeDevice MakeFake()
{
    FakeDevice fake{.vtable = kFakeVtable.data()};
    fake.files["splmods:/big.ytd"] = FakeFile{.sizeBytes = kLargeBytes, .magic = 0x37435352};
    fake.files["splmods:/small.ytd"] = FakeFile{.sizeBytes = 4096, .magic = 0x37435352};
    fake.files["splmods:/big.awc"] = FakeFile{.sizeBytes = kLargeBytes, .magic = 0x41444154};
    return fake;
}
} // namespace

TEST_CASE("LooseResourceDevice: a resource over 16 MiB reports the streamer's size marker",
          "[rage]")
{
    FakeDevice real = MakeFake();
    LooseResourceDevice device{&real};

    CHECK(CallSlot<LengthOfPathFn>(device, kSlotGetFileLengthLong, "splmods:/big.ytd") ==
          LooseResourceDevice::kLargeSizeMarker);
    CHECK(CallSlot<LengthOfPathFn>(device, kSlotGetFileLengthLong, "splmods:/small.ytd") == 4096);
    // Only a resource: other large files have no size header for the streamer to read.
    CHECK(CallSlot<LengthOfPathFn>(device, kSlotGetFileLengthLong, "splmods:/big.awc") ==
          kLargeBytes);
}

TEST_CASE("LooseResourceDevice: the 16-byte read at offset 0 of a large resource is its size",
          "[rage]")
{
    FakeDevice real = MakeFake();
    LooseResourceDevice device{&real};

    uint64_t offset = 0;
    const uint64_t handle =
        CallSlot<OpenBulkFn>(device, kSlotOpenBulk, "splmods:/big.ytd", &offset);
    REQUIRE(handle != kInvalidFileHandleRaw);
    CHECK(CallSlot<LengthOfHandleFn>(device, kSlotGetFileLengthUInt64, handle) ==
          LooseResourceDevice::kLargeSizeMarker);

    std::array<uint8_t, 16> header{};
    real.calls.clear();
    CHECK(CallSlot<ReadBulkFn>(device, kSlotReadBulk, handle, uint64_t{0}, header.data(),
                               uint32_t{16}) == 16);
    CHECK(real.calls.empty()); // answered without reading the file
    const uint32_t size = header[7] | (header[14] << 8) | (header[5] << 16) |
                          (static_cast<uint32_t>(header[2]) << 24);
    CHECK(size == kLargeBytes);

    // Every other read is the file's own bytes.
    std::array<uint8_t, 32> payload{};
    CHECK(CallSlot<ReadBulkFn>(device, kSlotReadBulk, handle, uint64_t{16}, payload.data(),
                               uint32_t{32}) == 32);
    CHECK(payload[0] == 'R');

    // Once closed, the handle number is nobody's large resource any more.
    CHECK(CallSlot<CloseFn>(device, kSlotCloseBulk, handle) == 0);
    CHECK_FALSE(device.FindHandle(handle));
}

TEST_CASE("LooseResourceDevice: small resources and unknown slots go to the real device", "[rage]")
{
    FakeDevice real = MakeFake();
    LooseResourceDevice device{&real};

    uint64_t offset = 0;
    const uint64_t handle =
        CallSlot<OpenBulkFn>(device, kSlotOpenBulk, "splmods:/small.ytd", &offset);
    std::array<uint8_t, 16> header{};
    real.calls.clear();
    CHECK(CallSlot<ReadBulkFn>(device, kSlotReadBulk, handle, uint64_t{0}, header.data(),
                               uint32_t{16}) == 16);
    CHECK(header[0] == 'R');
    CHECK(real.calls == std::vector<std::size_t>{kSlotReadBulk});

    real.calls.clear();
    using AttributesFn = uint64_t (*)(void*, const char*);
    CHECK(CallSlot<AttributesFn>(device, kSlotGetFileAttributes, "splmods:/small.ytd") ==
          0x1000 + kSlotGetFileAttributes);
    CHECK(real.calls == std::vector<std::size_t>{kSlotGetFileAttributes});
}
