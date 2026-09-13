#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rage/ForcedDevice.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"

using namespace spl::rage;
using namespace spl::rage::FileDeviceLayout;

namespace
{
constexpr std::size_t kFakeSlots = 64;

struct RecordedCall
{
    std::size_t slot = 0;
    std::array<uint64_t, 4> arguments{};
};

/// Stands in for the game's fiDeviceRelative: every slot records what it was called with and
/// answers 0x1000 + its slot number, so a test can tell which slot actually ran.
struct FakeDevice
{
    const void* const* vtable;
    std::vector<RecordedCall> calls;
};

template <std::size_t Slot>
uint64_t Record(void* self, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
{
    static_cast<FakeDevice*>(self)->calls.push_back(
        RecordedCall{.slot = Slot, .arguments = {first, second, third, fourth}});
    return 0x1000 + Slot;
}

const std::array<const void*, kFakeSlots> kFakeVtable =
    []<std::size_t... Slots>(std::index_sequence<Slots...>)
{
    return std::array<const void*, kFakeSlots>{reinterpret_cast<const void*>(&Record<Slots>)...};
}(std::make_index_sequence<kFakeSlots>{});

/// A call through the forced device's vtable, exactly the way game code makes it.
template <typename TFn, typename... TArgs>
auto CallSlot(ForcedDevice& device, std::size_t slot, TArgs&&... arguments)
{
    void* const object = device.GetGameDevice();
    const auto function = GetVirtualFunction<TFn>(reinterpret_cast<uintptr_t>(object), slot);
    return function(object, std::forward<TArgs>(arguments)...);
}

[[nodiscard]] std::string_view AsString(uint64_t argument)
{
    return reinterpret_cast<const char*>(argument);
}

constexpr std::string_view kForcedPath = "splres:/map_one/stream/map_one.ymf";
} // namespace

TEST_CASE("ForcedDevice: opening any name opens the forced path on the real device", "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    using OpenFn = uint64_t (*)(void*, const char*, bool);
    const uint64_t handle = CallSlot<OpenFn>(device, kSlotOpen, "localPack:/_manifest.ymf", true);

    CHECK(handle == 0x1000 + kSlotOpen);
    REQUIRE(real.calls.size() == 1);
    CHECK(real.calls[0].slot == kSlotOpen);
    CHECK(AsString(real.calls[0].arguments[0]) == kForcedPath);
    CHECK((real.calls[0].arguments[1] & 0xFF) == 1);
}

TEST_CASE("ForcedDevice: every path-taking slot substitutes the forced path", "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    using PathFn = uint64_t (*)(void*, const char*);
    for (const std::size_t slot :
         {kSlotCreateLocal, kSlotGetFileLengthLong, kSlotGetFileTime, kSlotGetFileAttributes})
    {
        real.calls.clear();
        (void)CallSlot<PathFn>(device, slot, "localPack:/whatever");
        REQUIRE(real.calls.size() == 1);
        CHECK(real.calls[0].slot == slot);
        CHECK(AsString(real.calls[0].arguments[0]) == kForcedPath);
    }

    real.calls.clear();
    using PathPointerFn = uint64_t (*)(void*, const char*, void*);
    int flags = 0;
    (void)CallSlot<PathPointerFn>(device, kSlotGetResourceVersion, "localPack:/x", &flags);
    REQUIRE(real.calls.size() == 1);
    CHECK(AsString(real.calls[0].arguments[0]) == kForcedPath);
    CHECK(real.calls[0].arguments[1] == reinterpret_cast<uint64_t>(&flags));

    // A bulk open goes to the real OpenBulk, wrapped or not.
    real.calls.clear();
    uint64_t bulkOffset = 0;
    using OpenBulkWrapFn = uint64_t (*)(void*, const char*, uint64_t*, void*);
    (void)CallSlot<OpenBulkWrapFn>(device, kSlotOpenBulkWrap, "localPack:/x", &bulkOffset, nullptr);
    REQUIRE(real.calls.size() == 1);
    CHECK(real.calls[0].slot == kSlotOpenBulk);
    CHECK(AsString(real.calls[0].arguments[0]) == kForcedPath);

    // The path is the third argument of this one.
    real.calls.clear();
    std::array<char, 16> buffer{};
    using ResolvePathFn = void* (*)(void*, void*, int, void*);
    (void)CallSlot<ResolvePathFn>(device, kSlotResolvePath, buffer.data(), 16, nullptr);
    REQUIRE(real.calls.size() == 1);
    CHECK((real.calls[0].arguments[1] & 0xFFFFFFFF) == 16);
    CHECK(AsString(real.calls[0].arguments[2]) == kForcedPath);
}

TEST_CASE("ForcedDevice: handle slots forward their arguments unchanged", "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    std::array<char, 16> buffer{};
    using ReadFn = uint32_t (*)(void*, uint64_t, void*, uint32_t);
    const uint32_t read = CallSlot<ReadFn>(device, kSlotRead, uint64_t{42}, buffer.data(), 16U);
    CHECK(read == 0x1000 + kSlotRead);
    REQUIRE(real.calls.size() == 1);
    CHECK(real.calls[0].arguments[0] == 42);
    CHECK(real.calls[0].arguments[1] == reinterpret_cast<uint64_t>(buffer.data()));
    CHECK((real.calls[0].arguments[2] & 0xFFFFFFFF) == 16);

    // ReadBulk has a fourth argument, which the caller passes on the stack.
    real.calls.clear();
    using ReadBulkFn = uint32_t (*)(void*, uint64_t, uint64_t, void*, uint32_t);
    (void)CallSlot<ReadBulkFn>(device, kSlotReadBulk, uint64_t{7}, uint64_t{0x800}, buffer.data(),
                               12U);
    REQUIRE(real.calls.size() == 1);
    CHECK(real.calls[0].slot == kSlotReadBulk);
    CHECK(real.calls[0].arguments[0] == 7);
    CHECK(real.calls[0].arguments[1] == 0x800);
    CHECK((real.calls[0].arguments[3] & 0xFFFFFFFF) == 12);

    real.calls.clear();
    using CloseFn = int32_t (*)(void*, uint64_t);
    (void)CallSlot<CloseFn>(device, kSlotClose, uint64_t{42});
    REQUIRE(real.calls.size() == 1);
    CHECK(real.calls[0].slot == kSlotClose);
    CHECK(real.calls[0].arguments[0] == 42);
}

TEST_CASE("ForcedDevice: slots it does not know, spare ones included, reach the real device",
          "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    using NoArgumentFn = uint64_t (*)(void*);
    CHECK((CallSlot<NoArgumentFn>(device, kSlotGetCollectionId) & 0xFFFFFFFF) ==
          0x1000 + kSlotGetCollectionId);
    CHECK(CallSlot<NoArgumentFn>(device, kKnownSlotCount + 3) == 0x1000 + kKnownSlotCount + 3);
    REQUIRE(real.calls.size() == 2);
}

TEST_CASE("ForcedDevice: writing is refused without touching the real device", "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    using CreateFn = uint64_t (*)(void*, const char*);
    CHECK(CallSlot<CreateFn>(device, kSlotCreate, "localPack:/new") == kInvalidFileHandleRaw);

    using WriteFn = uint32_t (*)(void*, uint64_t, void*, int);
    CHECK(CallSlot<WriteFn>(device, kSlotWrite, uint64_t{1}, nullptr, 0) == UINT32_MAX);

    using RemoveFn = bool (*)(void*, const char*);
    CHECK_FALSE(CallSlot<RemoveFn>(device, kSlotRemoveFile, "localPack:/x"));
    CHECK_FALSE(CallSlot<RemoveFn>(device, kSlotCreateDirectory, "localPack:/x"));

    CHECK(real.calls.empty());
}

TEST_CASE("ForcedDevice: reports its own name", "[rage]")
{
    FakeDevice real{.vtable = kFakeVtable.data()};
    ForcedDevice device(&real, std::string{kForcedPath});

    using GetNameFn = const char* (*)(void*);
    CHECK(std::string_view{CallSlot<GetNameFn>(device, kSlotGetName)} == ForcedDevice::kName);
    CHECK(real.calls.empty());
}
