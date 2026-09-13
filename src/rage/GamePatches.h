#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/Result.h"
#include "memory/CodePatch.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"
#include "rage/GameBuild.h"

namespace spl::rage
{
/// The code patches the loader leaves applied for the session, owned in one place so they can be
/// listed and restored together. Nothing is written until a stage asks for it, which keeps a
/// textures-only setup patch-free. Temporary patches (the map-store rebuild) are not in here.
class GamePatches
{
public:
    void Initialize(const GameAddresses& addresses, const GameBuild& build);

    /// Makes raw .ytyp files load the way FiveM's resources expect: nops the raw #typ loading
    /// check and makes fwMapTypesStore place synchronously. Idempotent; a refused patch leaves
    /// the other one in place and is reported.
    [[nodiscard]] Result<void> InstallMapTypesPatches();

    /// Makes raw .ymap and .ybn files usable after the session started: fwMapDataStore places
    /// synchronously, and box streamers accept new collision bounds.
    [[nodiscard]] Result<void> InstallMapDataPatches();

    [[nodiscard]] bool AreMapTypesPatchesInstalled() const
    {
        return m_rawCheckPatched && m_typesAsyncPlacePatched;
    }

    [[nodiscard]] bool AreMapDataPatchesInstalled() const
    {
        return m_dataAsyncPlacePatched && m_boundsReconfigPatched;
    }

    [[nodiscard]] std::size_t GetCount() const
    {
        return m_registry.GetCount();
    }

    /// One debug line per applied patch.
    void Dump() const
    {
        m_registry.Dump();
    }

    /// Puts every original byte back. Only safe once no game code can reach our replacements.
    void RestoreAll();

private:
    /// Nops a conditional short jump after checking that its opcode is still expectedOpcode and,
    /// when expectedDistance is given, that it still jumps that far.
    [[nodiscard]] Result<void> NopShortJump(const memory::Module& image, std::string_view name,
                                            uintptr_t address, uint8_t expectedOpcode,
                                            std::span<const uint8_t> expectedDistance);

    /// Points one "should async place" vtable slot at a function returning false.
    [[nodiscard]] Result<void> ReplaceShouldAsyncPlace(const memory::Module& image,
                                                       std::string_view name, uintptr_t vftable,
                                                       std::size_t unshiftedSlot);

    uintptr_t m_rawMapTypesLoadingCheck = 0;
    uintptr_t m_mapTypesStoreVftable = 0;
    uintptr_t m_mapDataStoreVftable = 0;
    uintptr_t m_boxStreamerBoundsReconfig = 0;
    uint32_t m_build = 0;
    bool m_rawCheckPatched = false;
    bool m_typesAsyncPlacePatched = false;
    bool m_dataAsyncPlacePatched = false;
    bool m_boundsReconfigPatched = false;
    memory::PatchRegistry m_registry;
};
} // namespace spl::rage
