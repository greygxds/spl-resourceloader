#include "rage/ContentInterface.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

#include "logging/Logger.h"
#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/ArchetypeTypes.h"
#include "util/Hash.h"

namespace spl::rage
{
void ContentInterface::Initialize(const GameAddresses& addresses)
{
    m_initGfxTexture = addresses.scaleformStoreInitGfxTexture;
    m_initPaintRamps = addresses.vehicleModelInfoInitPaintRamps;
    m_archetypeFactories = addresses.archetypeFactories;
    m_getAllPedArchetypes = addresses.pedModelInfoFactoryGetAllArchetypes;
}

Result<void> ContentInterface::InitScaleformSlot(LocalSlot slot, std::string_view streamingName)
{
    using InitGfxTextureFn = void (*)(int32_t localSlot, const char* name);

    if (m_initGfxTexture == 0)
    {
        return MakeError(ErrorCode::NotSupported,
                         "signature 'CScaleformStore::InitGfxTexture' did not resolve");
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }
    const std::string name{streamingName};
    const auto init = reinterpret_cast<InitGfxTextureFn>(m_initGfxTexture);
    if (!SafeCall("CScaleformStore::InitGfxTexture",
                  [&] { init(static_cast<int32_t>(slot.value), name.c_str()); }))
    {
        return MakeError(ErrorCode::Unavailable, "InitGfxTexture faulted for '{}'", name);
    }
    return {};
}

Result<void> ContentInterface::InitVehiclePaintRamps()
{
    using InitPaintRampsFn = void (*)();

    if (m_initPaintRamps == 0)
    {
        return MakeError(ErrorCode::NotSupported,
                         "signature 'CVehicleModelInfo::InitPaintRamps' did not resolve");
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }
    const auto init = reinterpret_cast<InitPaintRampsFn>(m_initPaintRamps);
    if (!SafeCall("CVehicleModelInfo::InitPaintRamps", [&] { init(); }))
    {
        return MakeError(ErrorCode::Unavailable, "InitPaintRamps faulted");
    }
    return {};
}

Result<std::size_t> ContentInterface::SetPedStreamFolders(std::span<const std::string> folders)
{
    using GetAllArchetypesFn = void (*)(void* factory, atArrayView<PedModelInfoView*>* out);

    if (m_archetypeFactories == 0 || m_getAllPedArchetypes == 0)
    {
        return MakeError(ErrorCode::NotSupported,
                         "signature 'fwArchetypeManager::ms_ArchetypeFactories' or "
                         "'CPedModelInfoFactory::GetAllArchetypes' did not resolve");
    }
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted");
    }

    // The layout is FiveM's, never checked on our builds, so every read is proven first.
    if (!IsReadableMemory(m_archetypeFactories, sizeof(atArrayView<void*>)))
    {
        return MakeError(ErrorCode::NotFound, "the archetype factory array is not readable");
    }
    const auto& factories = *reinterpret_cast<const atArrayView<void*>*>(m_archetypeFactories);
    if (factories.count <= ArchetypeLayout::kPedFactoryIndex || factories.data == nullptr ||
        !IsReadableMemory(reinterpret_cast<uintptr_t>(factories.data),
                          factories.count * sizeof(void*)))
    {
        return MakeError(ErrorCode::NotFound, "the archetype factory array has no ped factory");
    }
    void* const pedFactory = factories.data[ArchetypeLayout::kPedFactoryIndex];
    if (pedFactory == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "the ped archetype factory is empty");
    }

    // The game allocates the array; it is left to the game's heap, one small block per session.
    atArrayView<PedModelInfoView*> models{};
    const auto getAll = reinterpret_cast<GetAllArchetypesFn>(m_getAllPedArchetypes);
    if (!SafeCall("CPedModelInfoFactory::GetAllArchetypes", [&] { getAll(pedFactory, &models); }))
    {
        return MakeError(ErrorCode::Unavailable, "GetAllArchetypes faulted");
    }
    if (models.count > ArchetypeLayout::kMaxPedArchetypes ||
        (models.count > 0 &&
         !IsReadableMemory(reinterpret_cast<uintptr_t>(models.data), models.count * sizeof(void*))))
    {
        return MakeError(ErrorCode::NotFound, "the ped archetype list does not look like one");
    }

    std::size_t changed = 0;
    for (PedModelInfoView* const model : std::span(models.data, models.count))
    {
        if (!IsReadableMemory(reinterpret_cast<uintptr_t>(model), sizeof(PedModelInfoView)))
        {
            continue;
        }
        const auto folder = std::ranges::find_if(folders, [model](const std::string& name)
                                                 { return util::JoaatLower(name) == model->hash; });
        if (folder == folders.end() || folder->size() > ArchetypeLayout::kMaxStreamFolderLength)
        {
            continue;
        }

        atArrayView<char>& current = model->streamFolder;
        if (current.data != nullptr &&
            IsReadableMemory(reinterpret_cast<uintptr_t>(current.data), current.count) &&
            std::string_view{current.data, current.count} ==
                std::string_view{folder->c_str(), folder->size() + 1})
        {
            continue; // already pointed there, by the game's own data or an earlier session
        }
        if (current.data != nullptr || current.count != 0)
        {
            // The game's own peds have one; replacing a buffer the game allocated would leave it
            // to free ours later.
            SPL_LOG_DEBUG(Rage, "Ped model '{}' already has a stream folder; left as it is",
                          *folder);
            continue;
        }

        const std::string& stored = m_streamFolders.emplace_back(*folder);
        const auto length = static_cast<uint16_t>(stored.size() + 1);
        const bool written = SafeCall("CPedModelInfo::streamFolder",
                                      [&]
                                      {
                                          current.data = const_cast<char*>(stored.c_str());
                                          current.capacity = length;
                                          current.count = length;
                                      });
        if (!written)
        {
            return MakeError(ErrorCode::Unavailable, "writing the stream folder of '{}' faulted",
                             *folder);
        }
        ++changed;
    }
    return changed;
}
} // namespace spl::rage
