#include "rage/InteriorProxyPool.h"

#include <array>
#include <cstring>
#include <optional>

#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/MapStoreTypes.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
namespace
{
template <typename T> [[nodiscard]] std::optional<T> ReadValue(uintptr_t address)
{
    if (!IsReadableMemory(address, sizeof(T)) || HasFaulted())
    {
        return std::nullopt;
    }
    return SafeCall("CInteriorProxy::Read",
                    [&]
                    {
                        T value{};
                        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
                        return value;
                    });
}
} // namespace

void InteriorProxyPool::Initialize(const GameAddresses& addresses)
{
    m_poolPointer = addresses.interiorProxyPool;
}

std::optional<InteriorProxySnapshot> InteriorProxyPool::Read() const
{
    const std::optional<uintptr_t> poolAddress = ReadValue<uintptr_t>(m_poolPointer);
    if (!poolAddress)
    {
        return std::nullopt;
    }
    const std::optional<atPoolView> pool = ReadValue<atPoolView>(*poolAddress);
    if (!pool || pool->data == nullptr || pool->flags == nullptr ||
        pool->size > InteriorProxyLayout::kMaxPoolSize ||
        pool->entrySize < InteriorProxyLayout::kMinEntrySize ||
        !IsReadableMemory(reinterpret_cast<uintptr_t>(pool->flags), pool->size))
    {
        return std::nullopt;
    }

    InteriorProxySnapshot snapshot{.capacity = pool->size};
    for (uint32_t index = 0; index < pool->size; ++index)
    {
        const std::optional<int8_t> flag =
            ReadValue<int8_t>(reinterpret_cast<uintptr_t>(pool->flags) + index);
        if (!flag || *flag < 0)
        {
            continue; // negative means free
        }
        ++snapshot.used;

        const uintptr_t proxy = reinterpret_cast<uintptr_t>(pool->data) +
                                static_cast<uintptr_t>(index) * pool->entrySize;
        const std::optional<uint32_t> mapDataSlot =
            ReadValue<uint32_t>(proxy + InteriorProxyLayout::kMapDataSlot);
        const std::optional<std::array<float, 3>> position =
            ReadValue<std::array<float, 3>>(proxy + InteriorProxyLayout::kPosition);
        const std::optional<uint32_t> archetypeHash =
            ReadValue<uint32_t>(proxy + InteriorProxyLayout::kArchetypeHash);
        if (!mapDataSlot || !position || !archetypeHash)
        {
            continue;
        }
        snapshot.proxies.push_back(InteriorProxyInfo{.poolIndex = index,
                                                     .archetypeHash = *archetypeHash,
                                                     .mapDataSlot = *mapDataSlot,
                                                     .x = (*position)[0],
                                                     .y = (*position)[1],
                                                     .z = (*position)[2]});
    }
    return snapshot;
}
} // namespace spl::rage
