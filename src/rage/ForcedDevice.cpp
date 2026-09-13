#include "rage/ForcedDevice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"

namespace spl::rage
{
namespace
{
using namespace FileDeviceLayout;

/// Slots past the ones FiveM names. They forward like any unknown slot, so a build whose
/// fiDevice grew at the end still reaches the real device instead of a null pointer.
constexpr std::size_t kSpareSlots = 16;
constexpr std::size_t kVtableSlots = kKnownSlotCount + kSpareSlots;

/// Every fiDevice virtual takes at most four integer arguments after this. The fourth one of a
/// three-argument call is a stack slot in the caller's frame, so reading it is harmless.
using ForwardFn = uint64_t (*)(void* self, uint64_t, uint64_t, uint64_t, uint64_t);

[[nodiscard]] ForcedDevice& OwnerOf(void* self)
{
    return *static_cast<ForcedDeviceObject*>(self)->owner;
}

[[nodiscard]] void* RealOf(void* self)
{
    return OwnerOf(self).GetRealDevice();
}

[[nodiscard]] const char* ForcedPathOf(void* self)
{
    return OwnerOf(self).GetForcedPath();
}

/// The real device's implementation of slot, typed as TFn.
template <typename TFn> [[nodiscard]] TFn RealSlot(void* self, std::size_t slot)
{
    return GetVirtualFunction<TFn>(reinterpret_cast<uintptr_t>(RealOf(self)), slot);
}

template <std::size_t Slot>
uint64_t Forward(void* self, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
{
    return RealSlot<ForwardFn>(self, Slot)(RealOf(self), first, second, third, fourth);
}

/// One forwarder per slot: a slot number cannot travel through a call the game makes, so it
/// has to be baked into the function.
constexpr std::array<ForwardFn, kVtableSlots> kForwarders =
    []<std::size_t... Slots>(std::index_sequence<Slots...>)
{
    return std::array<ForwardFn, kVtableSlots>{&Forward<Slots>...};
}(std::make_index_sequence<kVtableSlots>{});

// The game never deletes a mounted device, and there is nothing of ours to free anyway.
void* Destructor(void* self, int /*flags*/)
{
    return self;
}

uint64_t Open(void* self, const char* /*fileName*/, bool readOnly)
{
    using Fn = uint64_t (*)(void*, const char*, bool);
    return RealSlot<Fn>(self, kSlotOpen)(RealOf(self), ForcedPathOf(self), readOnly);
}

uint64_t OpenBulk(void* self, const char* /*fileName*/, uint64_t* bulkOffset)
{
    using Fn = uint64_t (*)(void*, const char*, uint64_t*);
    return RealSlot<Fn>(self, kSlotOpenBulk)(RealOf(self), ForcedPathOf(self), bulkOffset);
}

// FiveM's ForcedDevice answers the wrapped open with a plain bulk open of the forced path.
uint64_t OpenBulkWrap(void* self, const char* fileName, uint64_t* bulkOffset, void* /*unused*/)
{
    return OpenBulk(self, fileName, bulkOffset);
}

/// Slots whose only argument is a path: CreateLocal, GetFileLengthLong, GetFileTime,
/// GetFileAttributes. A 32-bit answer is the low half of the 64-bit one.
template <std::size_t Slot> uint64_t WithForcedPath(void* self, const char* /*path*/)
{
    using Fn = uint64_t (*)(void*, const char*);
    return RealSlot<Fn>(self, Slot)(RealOf(self), ForcedPathOf(self));
}

/// Slots taking a path and one pointer: FindFirst, GetResourceVersion.
template <std::size_t Slot>
uint64_t WithForcedPathAndPointer(void* self, const char* /*path*/, void* pointer)
{
    using Fn = uint64_t (*)(void*, const char*, void*);
    return RealSlot<Fn>(self, Slot)(RealOf(self), ForcedPathOf(self), pointer);
}

void* ResolvePath(void* self, void* buffer, int length, void* /*path*/)
{
    using Fn = void* (*)(void*, void*, int, const char*);
    return RealSlot<Fn>(self, kSlotResolvePath)(RealOf(self), buffer, length, ForcedPathOf(self));
}

// The device is read-only: anything that would create or change a file fails the way FiveM's
// forced device fails it.
uint64_t RefuseHandle(void* /*self*/)
{
    return kInvalidFileHandleRaw;
}

uint32_t RefuseCount(void* /*self*/)
{
    return UINT32_MAX;
}

bool RefuseBool(void* /*self*/)
{
    return false;
}

const char* GetName(void* /*self*/)
{
    return ForcedDevice::kName.data(); // a string literal, so it is terminated
}

[[nodiscard]] std::array<const void*, kVtableSlots> BuildVtable()
{
    std::array<const void*, kVtableSlots> vtable{};
    for (std::size_t slot = 0; slot < kVtableSlots; ++slot)
    {
        vtable[slot] = reinterpret_cast<const void*>(kForwarders[slot]);
    }

    const auto set = [&vtable](std::size_t slot, auto function)
    { vtable[slot] = reinterpret_cast<const void*>(function); };

    set(kSlotDestructor, &Destructor);
    set(kSlotOpen, &Open);
    set(kSlotOpenBulk, &OpenBulk);
    set(kSlotOpenBulkWrap, &OpenBulkWrap);
    set(kSlotCreateLocal, &WithForcedPath<kSlotCreateLocal>);
    set(kSlotGetFileLengthLong, &WithForcedPath<kSlotGetFileLengthLong>);
    set(kSlotGetFileTime, &WithForcedPath<kSlotGetFileTime>);
    set(kSlotGetFileAttributes, &WithForcedPath<kSlotGetFileAttributes>);
    set(kSlotFindFirst, &WithForcedPathAndPointer<kSlotFindFirst>);
    set(kSlotGetResourceVersion, &WithForcedPathAndPointer<kSlotGetResourceVersion>);
    set(kSlotResolvePath, &ResolvePath);

    set(kSlotCreate, &RefuseHandle);
    set(kSlotWriteBulk, &RefuseCount);
    set(kSlotWrite, &RefuseCount);
    set(kSlotRemoveFile, &RefuseBool);
    set(kSlotRenameFile, &RefuseBool);
    set(kSlotCreateDirectory, &RefuseBool);
    set(kSlotRemoveDirectory, &RefuseBool);
    set(kSlotSetFileTime, &RefuseBool);
    set(kSlotTruncate, &RefuseBool);
    set(kSlotSetFileAttributes, &RefuseBool);
    set(kSlotWriteFull, &RefuseBool);

    set(kSlotGetName, &GetName);
    return vtable;
}

const std::array<const void*, kVtableSlots> g_forcedDeviceVtable = BuildVtable();
} // namespace

ForcedDevice::ForcedDevice(void* realDevice, std::string forcedPath)
    : m_object{.vtable = g_forcedDeviceVtable.data(), .owner = this}, m_realDevice(realDevice),
      m_forcedPath(std::move(forcedPath))
{
}
} // namespace spl::rage
