#include "rage/DataFileInterface.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/VirtualCall.h"
#include "streaming/DataFileType.h"
#include "util/Hash.h"
#include "util/Strings.h"

namespace spl::rage
{
namespace
{
using LoadDataFileFn = bool (*)(void* self, DataFileEntryView* entry);

/// The type a .ytyp request needs, and therefore the one Verify() insists on.
constexpr std::string_view kItypRequestType = "DLC_ITYP_REQUEST";

/// FiveM refuses it: "these don't work and will fail to unload" (LoadStreamingFile.cpp:1514).
constexpr std::string_view kTextFileMetafileType = "TEXTFILE_METAFILE";

[[nodiscard]] std::string FormatAddress(const memory::Module& image, uintptr_t address)
{
    if (!image.Contains(address))
    {
        return fmt::format("{:#x}", address);
    }
    return fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase());
}
} // namespace

Result<void> DataFileInterface::Initialize(const GameAddresses& addresses)
{
    m_typeTable = addresses.dataFileTypeTable;
    m_interfaces = addresses.dataFileMountInterfaces;
    if (m_typeTable == 0 || m_interfaces == 0)
    {
        return MakeError(ErrorCode::Unavailable, "the data-file addresses did not resolve");
    }
    return {};
}

std::optional<DataFileTypeIndex> DataFileInterface::LookupType(std::string_view typeName) const
{
    if (m_typeTable == 0 || HasFaulted())
    {
        return std::nullopt;
    }

    const uint32_t hash = util::JoaatExact(util::ToUpper(typeName));
    const auto* const rows = reinterpret_cast<const DataFileTypeEnumEntryView*>(m_typeTable);
    const std::optional<std::optional<uint32_t>> found =
        SafeCall("CDataFileMgr::sm_TypeTable",
                 [&]() -> std::optional<uint32_t>
                 {
                     for (std::size_t row = 0; row < DataFileLayout::kMaxTableRows; ++row)
                     {
                         const DataFileTypeEnumEntryView& entry = rows[row];
                         if (entry.hash == 0 && entry.index == DataFileLayout::kTableEndIndex)
                         {
                             break;
                         }
                         if (entry.hash == hash)
                         {
                             return entry.index;
                         }
                     }
                     return std::nullopt;
                 });
    if (!found || !*found || **found > static_cast<uint32_t>(INT32_MAX))
    {
        return std::nullopt;
    }
    return DataFileTypeIndex{static_cast<int32_t>(**found)};
}

void DataFileInterface::DumpMounters(const memory::Module& image) const
{
    if (m_typeTable == 0 || HasFaulted())
    {
        return;
    }

    const auto* const rows = reinterpret_cast<const DataFileTypeEnumEntryView*>(m_typeTable);
    const std::span<const streaming::DataFileTypeInfo> types = streaming::GetDataFileTypes();
    std::size_t typeCount = 0;
    std::size_t mounterCount = 0;
    std::string withoutMounter;
    for (std::size_t row = 0; row < DataFileLayout::kMaxTableRows; ++row)
    {
        const std::optional<DataFileTypeEnumEntryView> entry =
            SafeCall("CDataFileMgr::sm_TypeTable", [&] { return rows[row]; });
        if (!entry || (entry->hash == 0 && entry->index == DataFileLayout::kTableEndIndex))
        {
            break;
        }
        ++typeCount;

        const auto named =
            std::ranges::find_if(types, [&](const streaming::DataFileTypeInfo& info)
                                 { return util::JoaatExact(info.name) == entry->hash; });
        const std::string name = named != types.end() ? std::string{named->name}
                                                      : fmt::format("unknown {:08x}", entry->hash);

        void* const mounter =
            entry->index > static_cast<uint32_t>(INT32_MAX)
                ? nullptr
                : FindMounter(DataFileTypeIndex{static_cast<int32_t>(entry->index)});
        if (mounter == nullptr)
        {
            withoutMounter += fmt::format("{}{}", withoutMounter.empty() ? "" : ", ", name);
            SPL_LOG_DEBUG(Rage, "Data-file type {:3} {}: no mounter", entry->index, name);
            continue;
        }
        // A diagnostic must not fault: a caught fault stops every game call for the session.
        const auto object = reinterpret_cast<uintptr_t>(mounter);
        const std::optional<uintptr_t> vtable =
            IsReadableMemory(object, sizeof(uintptr_t))
                ? SafeCall("CDataFileMountInterface::vftable",
                           [&] { return GetVtableAddress(object); })
                : std::nullopt;
        if (!vtable || !image.Contains(*vtable))
        {
            SPL_LOG_DEBUG(Rage, "Data-file type {:3} {}: {:#x} is not a mounter", entry->index,
                          name, object);
            continue;
        }
        ++mounterCount;
        SPL_LOG_DEBUG(Rage, "Data-file type {:3} {}: mounter vtable {}", entry->index, name,
                      FormatAddress(image, *vtable));
    }

    SPL_LOG_DEBUG(Rage, "{} of {} data-file types have a mounter; without one: {}", mounterCount,
                  typeCount, withoutMounter.empty() ? std::string{"none"} : withoutMounter);
}

void* DataFileInterface::FindMounter(DataFileTypeIndex index) const
{
    if (m_interfaces == 0 || index.value < 0 || HasFaulted())
    {
        return nullptr;
    }
    const uintptr_t slot = m_interfaces + static_cast<uintptr_t>(index.value) * sizeof(void*);
    if (!IsReadableMemory(slot, sizeof(void*)))
    {
        return nullptr;
    }
    return SafeCall("CDataFileMount::sm_Interfaces",
                    [&] { return *reinterpret_cast<void* const*>(slot); })
        .value_or(nullptr);
}

Result<void> DataFileInterface::Verify(const memory::Module& image) const
{
    const std::optional<DataFileTypeIndex> index = LookupType(kItypRequestType);
    if (!index)
    {
        return MakeError(ErrorCode::NotFound,
                         "the data-file type table at {} does not list '{}', so it is not the "
                         "table we expect",
                         FormatAddress(image, m_typeTable), kItypRequestType);
    }

    void* const mounter = FindMounter(*index);
    if (mounter == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "data-file type '{}' (index {}) has no mounter",
                         kItypRequestType, index->value);
    }

    const std::optional<uintptr_t> loadDataFile =
        SafeCall("CDataFileMountInterface::vftable",
                 [&]
                 {
                     return GetVirtualFunction<uintptr_t>(reinterpret_cast<uintptr_t>(mounter),
                                                          DataFileLayout::kSlotLoadDataFile);
                 });
    if (!loadDataFile || !image.Contains(*loadDataFile))
    {
        return MakeError(
            ErrorCode::NotFound, "the '{}' mounter at {} does not look like a game object",
            kItypRequestType, FormatAddress(image, reinterpret_cast<uintptr_t>(mounter)));
    }

    SPL_LOG_DEBUG(Rage, "Data-file type '{}' is index {}, mounter LoadDataFile at {}",
                  kItypRequestType, index->value, FormatAddress(image, *loadDataFile));
    return {};
}

Result<void> DataFileInterface::Load(std::string_view typeName, std::string_view path)
{
    const std::string type = util::ToUpper(typeName);
    if (type == kTextFileMetafileType)
    {
        return MakeError(ErrorCode::NotSupported,
                         "data_file type '{}' is refused: the game cannot unload it", type);
    }
    const std::optional<DataFileTypeIndex> index = LookupType(type);
    if (!index)
    {
        return MakeError(ErrorCode::NotFound, "the game has no data file type '{}'", type);
    }
    void* const mounter = FindMounter(*index);
    if (mounter == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "no mounter for data file type '{}'", type);
    }

    Result<DataFileEntryView*> created = CreateEntry(type, path);
    if (!created)
    {
        return created.GetError();
    }
    DataFileEntryView* const entry = created.GetValue();

    const std::optional<bool> loaded =
        SafeCall("CDataFileMountInterface::LoadDataFile",
                 [&]
                 {
                     const auto load = GetVirtualFunction<LoadDataFileFn>(
                         reinterpret_cast<uintptr_t>(mounter), DataFileLayout::kSlotLoadDataFile);
                     return load(mounter, entry);
                 });
    if (!loaded)
    {
        return MakeError(ErrorCode::Unavailable, "mounting '{}' as {} faulted", path, type);
    }
    if (!*loaded)
    {
        return MakeError(ErrorCode::Unavailable, "the game's {} mounter refused '{}'", type, path);
    }
    return {};
}

Result<DataFileEntryView*> DataFileInterface::CreateEntry(std::string_view typeName,
                                                          std::string_view path)
{
    if (path.empty() || path.size() > DataFileLayout::kMaxNameLength)
    {
        return MakeError(ErrorCode::InvalidArgument,
                         "data file path '{}' does not fit the game's {}-character name", path,
                         DataFileLayout::kMaxNameLength);
    }
    const std::optional<DataFileTypeIndex> index = LookupType(typeName);
    if (!index)
    {
        return MakeError(ErrorCode::NotFound, "the game has no data file type '{}'", typeName);
    }

    auto& storage = m_entries.emplace_back(std::make_unique<EntryStorage>());
    auto* const entry = reinterpret_cast<DataFileEntryView*>(storage->data());
    std::memcpy(entry->name, path.data(), path.size()); // the storage is zeroed: terminated
    entry->type = index->value;
    return entry;
}

DataFileEntryView* DataFileInterface::FindEntry(DataFileTypeIndex index, std::string_view path)
{
    for (const std::unique_ptr<EntryStorage>& storage : m_entries)
    {
        auto* const entry = reinterpret_cast<DataFileEntryView*>(storage->data());
        if (entry->type == index.value && std::string_view{entry->name} == path)
        {
            return entry;
        }
    }
    return nullptr;
}

Result<void> DataFileInterface::Unload(std::string_view typeName, std::string_view path)
{
    using UnloadDataFileFn = void (*)(void* self, DataFileEntryView* entry);

    const std::string type = util::ToUpper(typeName);
    const std::optional<DataFileTypeIndex> index = LookupType(type);
    if (!index)
    {
        return MakeError(ErrorCode::NotFound, "the game has no data file type '{}'", type);
    }
    DataFileEntryView* const entry = FindEntry(*index, path);
    if (entry == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "'{}' was never loaded as {}", path, type);
    }
    void* const mounter = FindMounter(*index);
    if (mounter == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "no mounter for data file type '{}'", type);
    }

    // The storage stays alive: the mounter may still hold the pointer it was loaded with.
    const bool unloaded =
        SafeCall("CDataFileMountInterface::UnloadDataFile",
                 [&]
                 {
                     const auto unload = GetVirtualFunction<UnloadDataFileFn>(
                         reinterpret_cast<uintptr_t>(mounter), DataFileLayout::kSlotUnloadDataFile);
                     unload(mounter, entry);
                 });
    if (!unloaded)
    {
        return MakeError(ErrorCode::Unavailable, "unmounting '{}' as {} faulted", path, type);
    }
    return {};
}
} // namespace spl::rage
