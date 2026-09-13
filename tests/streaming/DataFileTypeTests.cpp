#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include <catch_amalgamated.hpp>

#include "config/LoaderConfig.h"
#include "streaming/DataFileType.h"

using spl::config::DataFileSettings;
using spl::streaming::DataFileCategory;
using spl::streaming::DataFileLoadOrder;
using spl::streaming::DataFilePolicy;
using spl::streaming::DataFileTypeInfo;
using spl::streaming::FindDataFileType;
using spl::streaming::GetDataFileTypes;
using spl::streaming::IsCategoryEnabled;

TEST_CASE("DataFileType: finds a type in any case", "[streaming]")
{
    const DataFileTypeInfo* const handling = FindDataFileType("handling_file");
    REQUIRE(handling != nullptr);
    CHECK(handling->name == "HANDLING_FILE");
    CHECK(handling->category == DataFileCategory::Vehicles);
    CHECK(FindDataFileType(" VEHICLE_METADATA_FILE ") != nullptr);
}

TEST_CASE("DataFileType: a name the game does not have is not found", "[streaming]")
{
    CHECK(FindDataFileType("") == nullptr);
    CHECK(FindDataFileType("CFX_PSEUDO_ENTRY") == nullptr);
    CHECK(FindDataFileType("NOT_A_DATA_FILE") == nullptr);
}

TEST_CASE("DataFileType: names are unique and upper case", "[streaming]")
{
    std::set<std::string_view> seen;
    for (const DataFileTypeInfo& info : GetDataFileTypes())
    {
        CHECK(seen.insert(info.name).second);
        CHECK(std::ranges::none_of(info.name, [](char c) { return c >= 'a' && c <= 'z'; }));
    }
    CHECK(seen.size() == GetDataFileTypes().size());
}

TEST_CASE("DataFileType: the reference indices match what the game reports", "[streaming]")
{
    // Build 3889 logged DLC_ITYP_REQUEST as index 174.
    CHECK(FindDataFileType("DLC_ITYP_REQUEST")->referenceIndex == 174);
    CHECK(FindDataFileType("TEXTFILE_METAFILE")->referenceIndex == 160);
    CHECK(FindDataFileType("INTERIOR_PROXY_ORDER_FILE")->referenceIndex == 173);
}

TEST_CASE("DataFileType: FiveM's exceptions carry their policy", "[streaming]")
{
    CHECK(FindDataFileType("DLC_ITYP_REQUEST")->policy == DataFilePolicy::TypeRequest);
    CHECK(FindDataFileType("TEXTFILE_METAFILE")->policy == DataFilePolicy::Refused);
    CHECK(FindDataFileType("GTXD_PARENTING_DATA")->policy == DataFilePolicy::Deferred);
    CHECK(FindDataFileType("RPF_FILE")->policy == DataFilePolicy::Packfile);
    CHECK(FindDataFileType("WEAPONINFO_FILE")->policy == DataFilePolicy::Generic);
    CHECK(FindDataFileType("AUDIO_GAMEDATA")->policy == DataFilePolicy::Generic);
}

TEST_CASE("DataFileType: only handling and vehicle layouts load first", "[streaming]")
{
    for (const DataFileTypeInfo& info : GetDataFileTypes())
    {
        const bool first = info.name == "HANDLING_FILE" || info.name == "VEHICLE_LAYOUTS_FILE";
        CHECK((info.loadOrder == DataFileLoadOrder::First) == first);
    }
}

TEST_CASE("DataFileType: the types real resources use are categorized", "[streaming]")
{
    CHECK(FindDataFileType("CARCOLS_FILE")->category == DataFileCategory::Vehicles);
    CHECK(FindDataFileType("VEHICLE_VARIATION_FILE")->category == DataFileCategory::Vehicles);
    CHECK(FindDataFileType("WEAPONCOMPONENTSINFO_FILE")->category == DataFileCategory::Weapons);
    CHECK(FindDataFileType("DLC_WEAPON_PICKUPS")->category == DataFileCategory::Weapons);
    CHECK(FindDataFileType("PED_METADATA_FILE")->category == DataFileCategory::Peds);
    CHECK(FindDataFileType("AUDIO_WAVEPACK")->category == DataFileCategory::Audio);
    CHECK(FindDataFileType("DLC_ITYP_REQUEST")->category == DataFileCategory::Maps);
    CHECK(FindDataFileType("CLIP_SETS_FILE")->category == DataFileCategory::Other);
}

TEST_CASE("DataFileType: categories follow their switches, maps always pass", "[streaming]")
{
    DataFileSettings settings;
    settings.vehicles = false;
    settings.other = false;

    CHECK_FALSE(IsCategoryEnabled(DataFileCategory::Vehicles, settings));
    CHECK_FALSE(IsCategoryEnabled(DataFileCategory::Other, settings));
    CHECK(IsCategoryEnabled(DataFileCategory::Weapons, settings));
    CHECK(IsCategoryEnabled(DataFileCategory::Maps, settings));
    CHECK(spl::streaming::ToString(DataFileCategory::Vehicles) == "vehicles");
}
