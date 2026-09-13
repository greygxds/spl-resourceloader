#include "rage/LooseResourceDevice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
using namespace FileDeviceLayout;

constexpr std::size_t kSpareSlots = 16;
constexpr std::size_t kVtableSlots = kKnownSlotCount + kSpareSlots;
constexpr uint32_t kRsc7Magic = 0x37435352;
constexpr uint32_t kHeaderSizeBytes = 16;

using ForwardFn = uint64_t (*)(void* self, uint64_t, uint64_t, uint64_t, uint64_t);

// NOLINTBEGIN(readability-identifier-naming): RAGE's own layout
/// rage::fiFindData, as FiveM's rage-device-five fiDevice.h declares it.
struct FindDataView
{
    char fileName[256];  // +0x000
    uint64_t fileSize;   // +0x100
    uint64_t writeTime;  // +0x108
    uint32_t attributes; // +0x110
};
// NOLINTEND(readability-identifier-naming)

[[nodiscard]] LooseResourceDevice& OwnerOf(void* self)
{
    return *static_cast<LooseResourceDeviceObject*>(self)->owner;
}

[[nodiscard]] void* RealOf(void* self)
{
    return OwnerOf(self).GetRealDevice();
}

template <typename TFn> [[nodiscard]] TFn RealSlotOf(void* realDevice, std::size_t slot)
{
    return GetVirtualFunction<TFn>(reinterpret_cast<uintptr_t>(realDevice), slot);
}

template <std::size_t Slot>
uint64_t Forward(void* self, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
{
    return RealSlotOf<ForwardFn>(RealOf(self), Slot)(RealOf(self), first, second, third, fourth);
}

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

uint64_t Open(void* self, const char* fileName, bool readOnly)
{
    using Fn = uint64_t (*)(void*, const char*, bool);
    const uint64_t handle =
        RealSlotOf<Fn>(RealOf(self), kSlotOpen)(RealOf(self), fileName, readOnly);
    if (handle != kInvalidFileHandleRaw)
    {
        if (const std::optional<uint32_t> size = OwnerOf(self).FindLargeResourceSize(fileName))
        {
            OwnerOf(self).RememberHandle(handle, *size);
        }
    }
    return handle;
}

uint64_t OpenBulk(void* self, const char* fileName, uint64_t* bulkOffset)
{
    using Fn = uint64_t (*)(void*, const char*, uint64_t*);
    const uint64_t handle =
        RealSlotOf<Fn>(RealOf(self), kSlotOpenBulk)(RealOf(self), fileName, bulkOffset);
    if (handle != kInvalidFileHandleRaw)
    {
        if (const std::optional<uint32_t> size = OwnerOf(self).FindLargeResourceSize(fileName))
        {
            OwnerOf(self).RememberHandle(handle, *size);
        }
    }
    return handle;
}

uint64_t OpenBulkWrap(void* self, const char* fileName, uint64_t* bulkOffset, void* extra)
{
    using Fn = uint64_t (*)(void*, const char*, uint64_t*, void*);
    const uint64_t handle =
        RealSlotOf<Fn>(RealOf(self), kSlotOpenBulkWrap)(RealOf(self), fileName, bulkOffset, extra);
    if (handle != kInvalidFileHandleRaw)
    {
        if (const std::optional<uint32_t> size = OwnerOf(self).FindLargeResourceSize(fileName))
        {
            OwnerOf(self).RememberHandle(handle, *size);
        }
    }
    return handle;
}

/// The streamer asks for the real size of a large resource with exactly this read.
uint32_t ReadBulk(void* self, uint64_t handle, uint64_t offset, void* buffer, uint32_t length)
{
    if (offset == 0 && length == kHeaderSizeBytes)
    {
        if (const std::optional<uint32_t> size = OwnerOf(self).FindHandle(handle))
        {
            std::array<uint8_t, kHeaderSizeBytes> header{};
            header[7] = static_cast<uint8_t>(*size & 0xFF);
            header[14] = static_cast<uint8_t>((*size >> 8) & 0xFF);
            header[5] = static_cast<uint8_t>((*size >> 16) & 0xFF);
            header[2] = static_cast<uint8_t>((*size >> 24) & 0xFF);
            std::memcpy(buffer, header.data(), header.size());
            return kHeaderSizeBytes;
        }
    }
    using Fn = uint32_t (*)(void*, uint64_t, uint64_t, void*, uint32_t);
    return RealSlotOf<Fn>(RealOf(self), kSlotReadBulk)(RealOf(self), handle, offset, buffer,
                                                       length);
}

template <std::size_t Slot> int32_t CloseAndForget(void* self, uint64_t handle)
{
    OwnerOf(self).ForgetHandle(handle);
    using Fn = int32_t (*)(void*, uint64_t);
    return RealSlotOf<Fn>(RealOf(self), Slot)(RealOf(self), handle);
}

template <std::size_t Slot> uint64_t LengthOfHandle(void* self, uint64_t handle)
{
    if (OwnerOf(self).FindHandle(handle))
    {
        return LooseResourceDevice::kLargeSizeMarker;
    }
    using Fn = uint64_t (*)(void*, uint64_t);
    return RealSlotOf<Fn>(RealOf(self), Slot)(RealOf(self), handle);
}

uint64_t GetFileLengthLong(void* self, const char* fileName)
{
    if (OwnerOf(self).FindLargeResourceSize(fileName))
    {
        return LooseResourceDevice::kLargeSizeMarker;
    }
    using Fn = uint64_t (*)(void*, const char*);
    return RealSlotOf<Fn>(RealOf(self), kSlotGetFileLengthLong)(RealOf(self), fileName);
}

uint64_t FindFirst(void* self, const char* path, FindDataView* findData)
{
    using Fn = uint64_t (*)(void*, const char*, FindDataView*);
    const uint64_t handle =
        RealSlotOf<Fn>(RealOf(self), kSlotFindFirst)(RealOf(self), path, findData);
    if (handle != kInvalidFileHandleRaw && findData != nullptr &&
        findData->fileSize >= LooseResourceDevice::kLargeSizeMarker &&
        OwnerOf(self).FindLargeResourceSize(path))
    {
        findData->fileSize = LooseResourceDevice::kLargeSizeMarker;
    }
    return handle;
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
    set(kSlotReadBulk, &ReadBulk);
    set(kSlotClose, &CloseAndForget<kSlotClose>);
    set(kSlotCloseBulk, &CloseAndForget<kSlotCloseBulk>);
    set(kSlotGetFileLength, &LengthOfHandle<kSlotGetFileLength>);
    set(kSlotGetFileLengthUInt64, &LengthOfHandle<kSlotGetFileLengthUInt64>);
    set(kSlotGetFileLengthLong, &GetFileLengthLong);
    set(kSlotFindFirst, &FindFirst);
    return vtable;
}

const std::array<const void*, kVtableSlots> g_looseResourceDeviceVtable = BuildVtable();
} // namespace

LooseResourceDevice::LooseResourceDevice(void* realDevice)
    : m_object{.vtable = g_looseResourceDeviceVtable.data(), .owner = this},
      m_realDevice(realDevice)
{
}

std::optional<uint32_t> LooseResourceDevice::FindLargeResourceSize(const char* path)
{
    if (path == nullptr)
    {
        return std::nullopt;
    }
    std::string key = util::ToLower(path);
    {
        const std::lock_guard lock{m_mutex};
        if (const auto known = m_sizes.find(key); known != m_sizes.end())
        {
            return known->second;
        }
    }

    // Asked of the real device directly, so our own detours never see these calls.
    std::optional<uint32_t> found;
    using LengthFn = uint64_t (*)(void*, const char*);
    const uint64_t length =
        RealSlotOf<LengthFn>(m_realDevice, kSlotGetFileLengthLong)(m_realDevice, path);
    if (length >= kLargeSizeMarker && length <= UINT32_MAX)
    {
        using OpenFn = uint64_t (*)(void*, const char*, bool);
        using ReadFn = uint32_t (*)(void*, uint64_t, void*, uint32_t);
        using CloseFn = int32_t (*)(void*, uint64_t);
        const uint64_t handle =
            RealSlotOf<OpenFn>(m_realDevice, kSlotOpen)(m_realDevice, path, true);
        if (handle != kInvalidFileHandleRaw)
        {
            uint32_t magic = 0;
            const uint32_t read = RealSlotOf<ReadFn>(m_realDevice, kSlotRead)(
                m_realDevice, handle, &magic, sizeof(magic));
            RealSlotOf<CloseFn>(m_realDevice, kSlotClose)(m_realDevice, handle);
            if (read == sizeof(magic) && magic == kRsc7Magic)
            {
                found = static_cast<uint32_t>(length);
            }
        }
    }

    const std::lock_guard lock{m_mutex};
    m_sizes.emplace(std::move(key), found);
    return found;
}

void LooseResourceDevice::RememberHandle(uint64_t handle, uint32_t sizeBytes)
{
    const std::lock_guard lock{m_mutex};
    m_handles[handle] = sizeBytes;
}

std::optional<uint32_t> LooseResourceDevice::FindHandle(uint64_t handle)
{
    const std::lock_guard lock{m_mutex};
    const auto found = m_handles.find(handle);
    return found != m_handles.end() ? std::optional<uint32_t>{found->second} : std::nullopt;
}

void LooseResourceDevice::ForgetHandle(uint64_t handle)
{
    const std::lock_guard lock{m_mutex};
    m_handles.erase(handle);
}
} // namespace spl::rage
