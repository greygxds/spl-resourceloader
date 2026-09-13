#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/Result.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"
#include "rage/types/DataFileTypes.h"

namespace spl::rage
{
/// Hands `data_file` entries to the game's own mounters, CDataFileMount::sm_Interfaces, the way
/// a DLC's content.xml would.
class DataFileInterface
{
public:
    [[nodiscard]] Result<void> Initialize(const GameAddresses& addresses);

    /// Proves the type table and the mounter array are what we think: DLC_ITYP_REQUEST has a
    /// type index, and its mounter is an object whose vtable sits in the image.
    [[nodiscard]] Result<void> Verify(const memory::Module& image) const;

    /// Logs every type in the game's table with its mounter, or "none" for a type that only
    /// loads at startup. Names come from streaming::GetDataFileTypes().
    void DumpMounters(const memory::Module& image) const;

    /// The index of a type name ("DLC_ITYP_REQUEST", any case), or std::nullopt when the game
    /// does not know it.
    [[nodiscard]] std::optional<DataFileTypeIndex> LookupType(std::string_view typeName) const;

    /// Mounts one data file. path goes into the entry's 128-byte name, so it must be shorter;
    /// the ytyp mounter only uses its base name.
    [[nodiscard]] Result<void> Load(std::string_view typeName, std::string_view path);

    /// A zeroed entry named path, of the given type, kept for the process like Load's. For the
    /// types whose mounting does not go through the game's mounter (RPF_FILE).
    [[nodiscard]] Result<DataFileEntryView*> CreateEntry(std::string_view typeName,
                                                         std::string_view path);

    /// Unmounts a data file loaded through Load, handing the mounter the entry it was loaded with.
    [[nodiscard]] Result<void> Unload(std::string_view typeName, std::string_view path);

private:
    using EntryStorage = std::array<std::byte, DataFileLayout::kEntryStorageBytes>;

    /// The entry Load kept for this name and type, or nullptr. Owned by m_entries.
    [[nodiscard]] DataFileEntryView* FindEntry(DataFileTypeIndex index, std::string_view path);

    /// The mounter for an index, or nullptr when the slot is empty. Owned by the game.
    [[nodiscard]] void* FindMounter(DataFileTypeIndex index) const;

    uintptr_t m_typeTable = 0;
    uintptr_t m_interfaces = 0;

    /// Entries stay alive for the process: nothing proves the mounter does not keep the pointer.
    std::vector<std::unique_ptr<EntryStorage>> m_entries;
};
} // namespace spl::rage
