#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>

#include "core/Result.h"
#include "rage/GameAddresses.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// The game calls some content needs after it is registered or mounted, beyond what the
/// streaming and data-file interfaces do: a new Scaleform slot, the vehicle paint
/// ramps, and the stream folder of an add-on ped. Every address is optional.
class ContentInterface
{
public:
    void Initialize(const GameAddresses& addresses);

    /// Prepares a .gfx slot the game did not have, which FiveM does for every new one
    /// (LoadStreamingFile.cpp:2034).
    [[nodiscard]] Result<void> InitScaleformSlot(LocalSlot slot, std::string_view streamingName);

    /// Rebuilds the vehicle paint ramps after a CARCOLS_FILE loaded. Only exists on 2545 and later.
    [[nodiscard]] Result<void> InitVehiclePaintRamps();

    /// Points every ped model whose name is in folders at its stream folder, so its components
    /// resolve under "<folder>/". Returns how many models were changed.
    [[nodiscard]] Result<std::size_t> SetPedStreamFolders(std::span<const std::string> folders);

private:
    uintptr_t m_initGfxTexture = 0;
    uintptr_t m_initPaintRamps = 0;
    uintptr_t m_archetypeFactories = 0;
    uintptr_t m_getAllPedArchetypes = 0;

    /// The folder names the game's models point at. A deque, so a new one never moves the old
    /// ones; they live for the process, like the add-on ped models that use them.
    std::deque<std::string> m_streamFolders;
};
} // namespace spl::rage
