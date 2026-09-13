#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "rage/GameAddresses.h"

namespace spl::rage
{
/// One CInteriorProxy: the placed MLO instance the game streams an interior from. Diagnostics only.
struct InteriorProxyInfo
{
    uint32_t poolIndex = 0;
    uint32_t archetypeHash = 0; ///< the CMloArchetypeDef's name hash
    uint32_t mapDataSlot = 0;   ///< the .ymap it came from, as a slot of the ymap store
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/// What the pool holds right now.
struct InteriorProxySnapshot
{
    uint32_t capacity = 0;
    uint32_t used = 0;
    std::vector<InteriorProxyInfo> proxies;
};

/// CInteriorProxy's pool, read-only. Every read is checked first, so a wrong layout on some
/// build gives an empty answer, never a fault.
class InteriorProxyPool
{
public:
    void Initialize(const GameAddresses& addresses);

    /// std::nullopt when the signature did not resolve or the pool is not readable.
    [[nodiscard]] std::optional<InteriorProxySnapshot> Read() const;

private:
    uintptr_t m_poolPointer = 0; ///< the static that holds the pool's address
};
} // namespace spl::rage
