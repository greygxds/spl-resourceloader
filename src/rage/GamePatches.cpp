#include "rage/GamePatches.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/SafeCall.h"
#include "rage/types/DataFileTypes.h"
#include "rage/types/MapStoreTypes.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
namespace
{
constexpr std::string_view kRawCheckPatchName = "RawMapTypesLoadingCheck";
constexpr std::string_view kTypesAsyncPlacePatchName = "MapTypesStoreSyncPlacement";
constexpr std::string_view kDataAsyncPlacePatchName = "MapDataStoreSyncPlacement";
constexpr std::string_view kBoundsReconfigPatchName = "BoxStreamerBoundsReconfig";

constexpr uint8_t kShortJzOpcode = 0x74;
constexpr uint8_t kShortJnzOpcode = 0x75;

/// FiveM CacheLoader.cpp:176 nops exactly "75 40".
constexpr std::array<uint8_t, 1> kBoundsReconfigDistance = {0x40};

/// What a store's "should async place" becomes (FiveM's ret0).
bool ShouldAsyncPlaceReplacement()
{
    return false;
}

[[nodiscard]] std::string FormatAddress(const memory::Module& image, uintptr_t address)
{
    if (!image.Contains(address))
    {
        return fmt::format("{:#x}", address);
    }
    return fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase());
}
} // namespace

void GamePatches::Initialize(const GameAddresses& addresses, const GameBuild& build)
{
    m_rawMapTypesLoadingCheck = addresses.rawMapTypesLoadingCheck;
    m_mapTypesStoreVftable = addresses.mapTypesStoreVftable;
    m_mapDataStoreVftable = addresses.mapDataStoreVftable;
    m_boxStreamerBoundsReconfig = addresses.boxStreamerBoundsReconfig;
    m_build = build.build;
}

Result<void> GamePatches::InstallMapTypesPatches()
{
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted; no patch is applied");
    }

    const memory::Module image = memory::Module::Main();
    if (!m_rawCheckPatched)
    {
        if (Result<void> patched = NopShortJump(image, kRawCheckPatchName,
                                                m_rawMapTypesLoadingCheck, kShortJzOpcode, {});
            !patched)
        {
            return patched;
        }
        m_rawCheckPatched = true;
    }
    if (!m_typesAsyncPlacePatched)
    {
        if (Result<void> patched =
                ReplaceShouldAsyncPlace(image, kTypesAsyncPlacePatchName, m_mapTypesStoreVftable,
                                        MapTypesStoreLayout::kSlotShouldAsyncPlace);
            !patched)
        {
            return patched;
        }
        m_typesAsyncPlacePatched = true;
    }
    return {};
}

Result<void> GamePatches::InstallMapDataPatches()
{
    if (HasFaulted())
    {
        return MakeError(ErrorCode::Unavailable, "a game call has faulted; no patch is applied");
    }

    const memory::Module image = memory::Module::Main();
    if (!m_dataAsyncPlacePatched)
    {
        if (Result<void> patched =
                ReplaceShouldAsyncPlace(image, kDataAsyncPlacePatchName, m_mapDataStoreVftable,
                                        MapDataStoreLayout::kSlotShouldAsyncPlace);
            !patched)
        {
            return patched;
        }
        m_dataAsyncPlacePatched = true;
    }
    if (!m_boundsReconfigPatched)
    {
        if (Result<void> patched =
                NopShortJump(image, kBoundsReconfigPatchName, m_boxStreamerBoundsReconfig,
                             kShortJnzOpcode, kBoundsReconfigDistance);
            !patched)
        {
            return patched;
        }
        m_boundsReconfigPatched = true;
    }
    return {};
}

Result<void> GamePatches::NopShortJump(const memory::Module& image, std::string_view name,
                                       uintptr_t address, uint8_t expectedOpcode,
                                       std::span<const uint8_t> expectedDistance)
{
    if (!image.Contains(address))
    {
        return MakeError(ErrorCode::NotFound, "patch '{}' has no address on this build", name);
    }

    const std::optional<std::array<uint8_t, 2>> current = SafeCall(
        "GamePatches::ReadShortJump",
        [&]
        {
            std::array<uint8_t, 2> bytes{};
            std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
            return bytes;
        });
    const bool opcodeMatches = current && (*current)[0] == expectedOpcode;
    const bool distanceMatches =
        expectedDistance.empty() || (current && (*current)[1] == expectedDistance[0]);
    if (!opcodeMatches || !distanceMatches)
    {
        return MakeError(ErrorCode::NotFound,
                         "patch '{}' refused: {} is not the expected short jump, so the signature "
                         "no longer points at the check",
                         name, FormatAddress(image, address));
    }

    const std::array<uint8_t, 2> nops = {0x90, 0x90};
    if (Result<void> applied = m_registry.Apply(std::string{name}, address, nops, *current);
        !applied)
    {
        return applied;
    }
    SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {}", name, FormatAddress(image, address));
    return {};
}

Result<void> GamePatches::ReplaceShouldAsyncPlace(const memory::Module& image,
                                                  std::string_view name, uintptr_t vftable,
                                                  std::size_t unshiftedSlot)
{
    if (!image.Contains(vftable))
    {
        return MakeError(ErrorCode::NotFound, "patch '{}' has no vtable on this build", name);
    }

    const std::size_t slot = unshiftedSlot + StreamingModuleLayout::VtableShift(m_build);
    const uintptr_t address = vftable + slot * sizeof(uintptr_t);

    const std::optional<uintptr_t> original =
        SafeCall("GamePatches::ReadVtableSlot",
                 [&] { return *reinterpret_cast<const uintptr_t*>(address); });
    if (!original || !image.Contains(*original))
    {
        return MakeError(ErrorCode::NotFound,
                         "patch '{}' refused: vtable slot {} does not point into {}, so the slot "
                         "index is wrong for build {}",
                         name, slot, image.GetFileName(), m_build);
    }

    const auto replacement = reinterpret_cast<uintptr_t>(&ShouldAsyncPlaceReplacement);
    std::array<uint8_t, sizeof(uintptr_t)> bytes{};
    std::array<uint8_t, sizeof(uintptr_t)> expected{};
    std::memcpy(bytes.data(), &replacement, bytes.size());
    std::memcpy(expected.data(), &*original, expected.size());

    if (Result<void> applied = m_registry.Apply(std::string{name}, address, bytes, expected);
        !applied)
    {
        return applied;
    }
    SPL_LOG_DEBUG(Hook, "Patch '{}' applied at {} (slot {})", name, FormatAddress(image, address),
                  slot);
    return {};
}

void GamePatches::RestoreAll()
{
    m_registry.RestoreAll();
    m_rawCheckPatched = false;
    m_typesAsyncPlacePatched = false;
    m_dataAsyncPlacePatched = false;
    m_boundsReconfigPatched = false;
}
} // namespace spl::rage
