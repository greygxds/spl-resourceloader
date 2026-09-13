#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "config/LoaderConfig.h"

namespace spl::streaming
{
/// How a `data_file` type is handled. FiveM has no whitelist: any type the game knows goes to
/// the game's own mounter, with a handful of exceptions (LoadStreamingFile.cpp:1489-1531).
enum class DataFilePolicy : uint8_t
{
    Generic,     ///< CDataFileMount::sm_Interfaces[type]->LoadDataFile, as FiveM does
    TypeRequest, ///< DLC_ITYP_REQUEST: its .ytyp has to be registered first
    Refused,     ///< TEXTFILE_METAFILE: FiveM refuses it, the game cannot handle it at runtime
    Deferred,    ///< GTXD_PARENTING_DATA: FiveM mounts it after the map store reload
    Packfile     ///< RPF_FILE: FiveM's packfile mounter, which goes through a manifest chunk
};

/// Groups of types a user can switch off together ([data_files]).
enum class DataFileCategory : uint8_t
{
    Maps, ///< not switchable here: [streaming] load_maps and the ytyp it needs decide
    Vehicles,
    Weapons,
    Peds,
    Audio,
    Other
};

/// FiveM's dfSort (LoadStreamingFile.cpp:2425-2443): these first, everything else after, in
/// manifest order.
enum class DataFileLoadOrder : uint8_t
{
    First = 0,
    Normal = 100
};

/// One type the game's CDataFileMgr knows.
struct DataFileTypeInfo
{
    std::string_view name; ///< upper case, as the game hashes it

    /// The index in the game's type enum as CodeWalker records it (PsoTypes.cs, DataFileType).
    /// Informational: the game's own table decides at runtime.
    int32_t referenceIndex = -1;

    DataFileCategory category = DataFileCategory::Other;
    DataFilePolicy policy = DataFilePolicy::Generic;
    DataFileLoadOrder loadOrder = DataFileLoadOrder::Normal;
};

/// The row for a type name, in any case, or nullptr when the game has no such type.
[[nodiscard]] const DataFileTypeInfo* FindDataFileType(std::string_view name);

/// Every known type, in the game's enum order.
[[nodiscard]] std::span<const DataFileTypeInfo> GetDataFileTypes();

/// True when [data_files] lets this category load. Maps always pass: their gate is the ytyp's.
[[nodiscard]] bool IsCategoryEnabled(DataFileCategory category,
                                     const config::DataFileSettings& settings);

/// The config key behind a category, for the message that explains a skip: "vehicles".
[[nodiscard]] std::string_view ToString(DataFileCategory category);

/// For logs: "generic", "refused", ...
[[nodiscard]] std::string_view ToString(DataFilePolicy policy);
} // namespace spl::streaming
