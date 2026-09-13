#include "streaming/DataFileType.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

#include "config/LoaderConfig.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
using enum DataFileCategory;
using enum DataFilePolicy;

/// Every type in the game's CDataFileMgr enum, from CodeWalker
/// (CodeWalker.Core/GameFiles/MetaTypes/PsoTypes.cs, DataFileType). FiveM's exceptions are the
/// rows with a policy; HANDLING_FILE and VEHICLE_LAYOUTS_FILE load first (dfSort).
constexpr std::array kDataFileTypes = std::to_array<DataFileTypeInfo>({
    {.name = "RPF_FILE", .referenceIndex = 0, .category = Other, .policy = Packfile},
    {.name = "IDE_FILE", .referenceIndex = 1, .category = Maps},
    {.name = "DELAYED_IDE_FILE", .referenceIndex = 2, .category = Maps},
    {.name = "IPL_FILE", .referenceIndex = 3, .category = Maps},
    {.name = "PERMANENT_ITYP_FILE", .referenceIndex = 4, .category = Maps},
    {.name = "HANDLING_FILE",
     .referenceIndex = 6,
     .category = Vehicles,
     .loadOrder = DataFileLoadOrder::First},
    {.name = "VEHICLEEXTRAS_FILE", .referenceIndex = 7, .category = Vehicles},
    {.name = "CHINESE_SHUFFLING_CHECKS_FILE", .referenceIndex = 8, .category = Other},
    {.name = "PEDSTREAM_FILE", .referenceIndex = 9, .category = Peds},
    {.name = "CARCOLS_FILE", .referenceIndex = 10, .category = Vehicles},
    {.name = "POPGRP_FILE", .referenceIndex = 11, .category = Other},
    {.name = "POPSCHED_FILE", .referenceIndex = 14, .category = Other},
    {.name = "ZONEBIND_FILE", .referenceIndex = 15, .category = Other},
    {.name = "RADIO_FILE", .referenceIndex = 16, .category = Other},
    {.name = "EXTRAMAP_CONVERSION_OCC_FILE", .referenceIndex = 17, .category = Other},
    {.name = "THROWNWEAPONINFO_FILE", .referenceIndex = 18, .category = Weapons},
    {.name = "RMPTFX_FILE", .referenceIndex = 19, .category = Other},
    {.name = "PED_PERSONALITY_FILE", .referenceIndex = 20, .category = Peds},
    {.name = "PED_PERCEPTION_FILE", .referenceIndex = 21, .category = Peds},
    {.name = "VEHICLE_CAMERA_OFFSETS_FILE", .referenceIndex = 22, .category = Vehicles},
    {.name = "FRONTEND_MENU_FILE", .referenceIndex = 23, .category = Other},
    {.name = "LEADERBOARD_DATA_FILE", .referenceIndex = 24, .category = Other},
    {.name = "LEADERBOARD_ICONS_FILE", .referenceIndex = 25, .category = Other},
    {.name = "NETWORKOPTIONS_FILE", .referenceIndex = 26, .category = Other},
    {.name = "TIMECYCLE_FILE", .referenceIndex = 27, .category = Other},
    {.name = "TIMECYCLEMOD_FILE", .referenceIndex = 28, .category = Other},
    {.name = "WEATHER_FILE", .referenceIndex = 29, .category = Other},
    {.name = "PROCOBJ_FILE", .referenceIndex = 32, .category = Other},
    {.name = "PROC_META_FILE", .referenceIndex = 33, .category = Other},
    {.name = "VFX_SETTINGS_FILE", .referenceIndex = 34, .category = Other},
    {.name = "SP_STATS_DISPLAY_LIST_FILE", .referenceIndex = 35, .category = Other},
    {.name = "MP_STATS_DISPLAY_LIST_FILE", .referenceIndex = 36, .category = Other},
    {.name = "PED_VARS_FILE", .referenceIndex = 37, .category = Peds},
    {.name = "DISABLE_FILE", .referenceIndex = 38, .category = Other},
    {.name = "BUILDING_META_DISPLACEMENT_FILE", .referenceIndex = 39, .category = Other},
    {.name = "HUD_TXD_FILE", .referenceIndex = 40, .category = Other},
    {.name = "FRONTEND_DAT_FILE", .referenceIndex = 41, .category = Other},
    {.name = "SCROLLBARS_FILE", .referenceIndex = 42, .category = Other},
    {.name = "TIME_FILE", .referenceIndex = 43, .category = Other},
    {.name = "BLOODFX_FILE", .referenceIndex = 44, .category = Other},
    {.name = "ENTITYFX_FILE", .referenceIndex = 45, .category = Other},
    {.name = "EXPLOSIONFX_FILE", .referenceIndex = 46, .category = Other},
    {.name = "MATERIALFX_FILE", .referenceIndex = 47, .category = Other},
    {.name = "MOTION_TASK_DATA_FILE", .referenceIndex = 48, .category = Other},
    {.name = "DEFAULT_TASK_DATA_FILE", .referenceIndex = 49, .category = Other},
    {.name = "MOUNT_TUNE_FILE", .referenceIndex = 50, .category = Other},
    {.name = "PED_BOUNDS_FILE", .referenceIndex = 51, .category = Peds},
    {.name = "PED_HEALTH_FILE", .referenceIndex = 52, .category = Peds},
    {.name = "PED_COMPONENT_SETS_FILE", .referenceIndex = 53, .category = Peds},
    {.name = "PED_IK_SETTINGS_FILE", .referenceIndex = 54, .category = Peds},
    {.name = "PED_TASK_DATA_FILE", .referenceIndex = 55, .category = Peds},
    {.name = "PED_SPECIAL_ABILITIES_FILE", .referenceIndex = 56, .category = Peds},
    {.name = "WHEELFX_FILE", .referenceIndex = 57, .category = Other},
    {.name = "WEAPONFX_FILE", .referenceIndex = 58, .category = Weapons},
    {.name = "DECALS_FILE", .referenceIndex = 59, .category = Other},
    {.name = "NAVMESH_INDEXREMAPPING_FILE", .referenceIndex = 60, .category = Other},
    {.name = "NAVNODE_INDEXREMAPPING_FILE", .referenceIndex = 61, .category = Other},
    {.name = "AUDIOMESH_INDEXREMAPPING_FILE", .referenceIndex = 62, .category = Other},
    {.name = "JUNCTION_TEMPLATES_FILE", .referenceIndex = 63, .category = Other},
    {.name = "PATH_ZONES_FILE", .referenceIndex = 64, .category = Other},
    {.name = "DISTANT_LIGHTS_FILE", .referenceIndex = 65, .category = Other},
    {.name = "DISTANT_LIGHTS_HD_FILE", .referenceIndex = 66, .category = Other},
    {.name = "FLIGHTZONES_FILE", .referenceIndex = 67, .category = Other},
    {.name = "WATER_FILE", .referenceIndex = 68, .category = Other},
    {.name = "TRAINCONFIGS_FILE", .referenceIndex = 69, .category = Other},
    {.name = "TRAINTRACK_FILE", .referenceIndex = 70, .category = Other},
    {.name = "PED_METADATA_FILE", .referenceIndex = 71, .category = Peds},
    {.name = "WEAPON_METADATA_FILE", .referenceIndex = 72, .category = Weapons},
    {.name = "VEHICLE_METADATA_FILE", .referenceIndex = 73, .category = Vehicles},
    {.name = "DISPATCH_DATA_FILE", .referenceIndex = 74, .category = Other},
    {.name = "DEFORMABLE_OBJECTS_FILE", .referenceIndex = 75, .category = Other},
    {.name = "TUNABLE_OBJECTS_FILE", .referenceIndex = 76, .category = Other},
    {.name = "PED_NAV_CAPABILITES_FILE", .referenceIndex = 77, .category = Peds},
    {.name = "WEAPONINFO_FILE", .referenceIndex = 78, .category = Weapons},
    {.name = "WEAPONCOMPONENTSINFO_FILE", .referenceIndex = 79, .category = Weapons},
    {.name = "LOADOUTS_FILE", .referenceIndex = 80, .category = Weapons},
    {.name = "FIRINGPATTERNS_FILE", .referenceIndex = 81, .category = Weapons},
    {.name = "MOTIVATIONS_FILE", .referenceIndex = 82, .category = Other},
    {.name = "SCENARIO_POINTS_FILE", .referenceIndex = 83, .category = Maps},
    {.name = "SCENARIO_POINTS_PSO_FILE", .referenceIndex = 84, .category = Maps},
    {.name = "STREAMING_FILE", .referenceIndex = 85, .category = Other},
    {.name = "STREAMING_FILE_PLATFORM_PS3", .referenceIndex = 86, .category = Other},
    {.name = "STREAMING_FILE_PLATFORM_XENON", .referenceIndex = 87, .category = Other},
    {.name = "STREAMING_FILE_PLATFORM_OTHER", .referenceIndex = 88, .category = Other},
    {.name = "PED_BRAWLING_STYLE_FILE", .referenceIndex = 89, .category = Peds},
    {.name = "AMBIENT_PED_MODEL_SET_FILE", .referenceIndex = 90, .category = Peds},
    {.name = "AMBIENT_PROP_MODEL_SET_FILE", .referenceIndex = 91, .category = Maps},
    {.name = "AMBIENT_VEHICLE_MODEL_SET_FILE", .referenceIndex = 92, .category = Vehicles},
    {.name = "LADDER_METADATA_FILE", .referenceIndex = 93, .category = Other},
    {.name = "SLOWNESS_ZONES_FILE", .referenceIndex = 95, .category = Other},
    {.name = "LIQUIDFX_FILE", .referenceIndex = 96, .category = Other},
    {.name = "VFXVEHICLEINFO_FILE", .referenceIndex = 97, .category = Vehicles},
    {.name = "VFXPEDINFO_FILE", .referenceIndex = 98, .category = Other},
    {.name = "DOOR_TUNING_FILE", .referenceIndex = 99, .category = Other},
    {.name = "PTFXASSETINFO_FILE", .referenceIndex = 100, .category = Other},
    {.name = "SCRIPTFX_FILE", .referenceIndex = 101, .category = Other},
    {.name = "VFXREGIONINFO_FILE", .referenceIndex = 102, .category = Other},
    {.name = "VFXINTERIORINFO_FILE", .referenceIndex = 103, .category = Other},
    {.name = "CAMERA_METADATA_FILE", .referenceIndex = 104, .category = Other},
    {.name = "STREET_VEHICLE_ASSOCIATION_FILE", .referenceIndex = 105, .category = Vehicles},
    {.name = "VFXWEAPONINFO_FILE", .referenceIndex = 106, .category = Weapons},
    {.name = "EXPLOSION_INFO_FILE", .referenceIndex = 107, .category = Other},
    {.name = "JUNCTION_TEMPLATES_PSO_FILE", .referenceIndex = 108, .category = Other},
    {.name = "MAPZONES_FILE", .referenceIndex = 109, .category = Other},
    {.name = "SP_STATS_UI_LIST_FILE", .referenceIndex = 110, .category = Other},
    {.name = "MP_STATS_UI_LIST_FILE", .referenceIndex = 111, .category = Other},
    {.name = "OBJ_COVER_TUNING_FILE", .referenceIndex = 112, .category = Other},
    {.name = "STREAMING_REQUEST_LISTS_FILE", .referenceIndex = 113, .category = Other},
    {.name = "PLAYER_CARD_SETUP", .referenceIndex = 114, .category = Other},
    {.name = "WORLD_HEIGHTMAP_FILE", .referenceIndex = 115, .category = Other},
    {.name = "WORLD_WATERHEIGHT_FILE", .referenceIndex = 116, .category = Other},
    {.name = "PED_OVERLAY_FILE", .referenceIndex = 117, .category = Peds},
    {.name = "WEAPON_ANIMATIONS_FILE", .referenceIndex = 118, .category = Weapons},
    {.name = "VEHICLE_POPULATION_FILE", .referenceIndex = 119, .category = Vehicles},
    {.name = "ACTION_TABLE_DEFINITIONS", .referenceIndex = 120, .category = Other},
    {.name = "ACTION_TABLE_RESULTS", .referenceIndex = 121, .category = Other},
    {.name = "ACTION_TABLE_IMPULSES", .referenceIndex = 122, .category = Other},
    {.name = "ACTION_TABLE_RUMBLES", .referenceIndex = 123, .category = Other},
    {.name = "ACTION_TABLE_INTERRELATIONS", .referenceIndex = 124, .category = Other},
    {.name = "ACTION_TABLE_HOMINGS", .referenceIndex = 125, .category = Other},
    {.name = "ACTION_TABLE_DAMAGES", .referenceIndex = 126, .category = Other},
    {.name = "ACTION_TABLE_STRIKE_BONES", .referenceIndex = 127, .category = Other},
    {.name = "ACTION_TABLE_BRANCHES", .referenceIndex = 128, .category = Other},
    {.name = "ACTION_TABLE_STEALTH_KILLS", .referenceIndex = 129, .category = Other},
    {.name = "ACTION_TABLE_VFX", .referenceIndex = 130, .category = Other},
    {.name = "ACTION_TABLE_FACIAL_ANIM_SETS", .referenceIndex = 131, .category = Other},
    {.name = "VEHGEN_MARKUP_FILE", .referenceIndex = 132, .category = Other},
    {.name = "PED_COMPONENT_CLOTH_FILE", .referenceIndex = 133, .category = Peds},
    {.name = "TATTOO_SHOP_DLC_FILE", .referenceIndex = 134, .category = Peds},
    {.name = "VEHICLE_VARIATION_FILE", .referenceIndex = 135, .category = Vehicles},
    {.name = "CONTENT_UNLOCKING_META_FILE", .referenceIndex = 136, .category = Other},
    {.name = "SHOP_PED_APPAREL_META_FILE", .referenceIndex = 137, .category = Peds},
    {.name = "AUDIO_SOUNDDATA", .referenceIndex = 138, .category = Audio},
    {.name = "AUDIO_CURVEDATA", .referenceIndex = 139, .category = Audio},
    {.name = "AUDIO_GAMEDATA", .referenceIndex = 140, .category = Audio},
    {.name = "AUDIO_DYNAMIXDATA", .referenceIndex = 141, .category = Audio},
    {.name = "AUDIO_SPEECHDATA", .referenceIndex = 142, .category = Audio},
    {.name = "AUDIO_SYNTHDATA", .referenceIndex = 143, .category = Audio},
    {.name = "AUDIO_WAVEPACK", .referenceIndex = 144, .category = Audio},
    {.name = "CLIP_SETS_FILE", .referenceIndex = 145, .category = Other},
    {.name = "EXPRESSION_SETS_FILE", .referenceIndex = 146, .category = Peds},
    {.name = "FACIAL_CLIPSET_GROUPS_FILE", .referenceIndex = 147, .category = Peds},
    {.name = "VEHICLE_SHOP_DLC_FILE", .referenceIndex = 149, .category = Vehicles},
    {.name = "WEAPON_SHOP_INFO_METADATA_FILE", .referenceIndex = 150, .category = Weapons},
    {.name = "SCALEFORM_PREALLOC_FILE", .referenceIndex = 151, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE", .referenceIndex = 152, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_360", .referenceIndex = 153, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_PS3", .referenceIndex = 154, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_PS3_JPN", .referenceIndex = 155, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_ORBIS", .referenceIndex = 156, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_ORBIS_JPN", .referenceIndex = 157, .category = Other},
    {.name = "CONTROLLER_LABELS_FILE_DURANGO", .referenceIndex = 158, .category = Other},
    {.name = "TEXTFILE_METAFILE", .referenceIndex = 160, .category = Other, .policy = Refused},
    {.name = "NM_TUNING_FILE", .referenceIndex = 161, .category = Other},
    {.name = "MOVE_NETWORK_DEFS", .referenceIndex = 162, .category = Other},
    {.name = "WEAPONINFO_FILE_PATCH", .referenceIndex = 163, .category = Weapons},
    {.name = "DLC_SCRIPT_METAFILE", .referenceIndex = 164, .category = Other},
    {.name = "VEHICLE_LAYOUTS_FILE",
     .referenceIndex = 165,
     .category = Vehicles,
     .loadOrder = DataFileLoadOrder::First},
    {.name = "DLC_WEAPON_PICKUPS", .referenceIndex = 166, .category = Weapons},
    {.name = "EXTRA_TITLE_UPDATE_DATA", .referenceIndex = 167, .category = Other},
    {.name = "SCALEFORM_DLC_FILE", .referenceIndex = 168, .category = Other},
    {.name = "OVERLAY_INFO_FILE", .referenceIndex = 169, .category = Other},
    {.name = "ALTERNATE_VARIATIONS_FILE", .referenceIndex = 170, .category = Peds},
    {.name = "HORSE_REINS_FILE", .referenceIndex = 171, .category = Other},
    {.name = "FIREFX_FILE", .referenceIndex = 172, .category = Other},
    {.name = "INTERIOR_PROXY_ORDER_FILE", .referenceIndex = 173, .category = Maps},
    {.name = "DLC_ITYP_REQUEST", .referenceIndex = 174, .category = Maps, .policy = TypeRequest},
    {.name = "EXTRA_FOLDER_MOUNT_DATA", .referenceIndex = 175, .category = Other},
    {.name = "AMB_PROCEDURAL_BLOOD_FILE", .referenceIndex = 176, .category = Other},
    {.name = "SCRIPT_BRAIN_FILE", .referenceIndex = 177, .category = Other},
    {.name = "SCALEFORM_VALID_METHODS_FILE", .referenceIndex = 178, .category = Other},
    {.name = "DLC_POP_GROUPS", .referenceIndex = 179, .category = Other},
    {.name = "SCENARIO_INFO_FILE", .referenceIndex = 181, .category = Other},
    {.name = "CONDITIONAL_ANIMS_FILE", .referenceIndex = 182, .category = Other},
    {.name = "STATS_METADATA_PSO_FILE", .referenceIndex = 183, .category = Other},
    {.name = "VFXFOGVOLUMEINFO_FILE", .referenceIndex = 184, .category = Other},
    {.name = "RPF_FILE_PRE_INSTALL", .referenceIndex = 185, .category = Other},
    {.name = "LEVEL_STREAMING_FILE", .referenceIndex = 187, .category = Other},
    {.name = "SCENARIO_POINTS_OVERRIDE_FILE", .referenceIndex = 188, .category = Maps},
    {.name = "DRIVER_RULES_STD_FILE", .referenceIndex = 190, .category = Other},
    {.name = "PED_FIRST_PERSON_ASSET_DATA", .referenceIndex = 191, .category = Peds},
    {.name = "GTXD_PARENTING_DATA", .referenceIndex = 192, .category = Maps, .policy = Deferred},
    {.name = "COMBAT_BEHAVIOUR_OVERRIDE_FILE", .referenceIndex = 193, .category = Other},
    {.name = "EVENTS_OVERRIDE_FILE", .referenceIndex = 194, .category = Other},
    {.name = "PED_DAMAGE_OVERRIDE_FILE", .referenceIndex = 195, .category = Peds},
    {.name = "PED_DAMAGE_APPEND_FILE", .referenceIndex = 196, .category = Peds},
    {.name = "BACKGROUND_SCRIPT_FILE", .referenceIndex = 197, .category = Other},
    {.name = "PS3_SCRIPT_RPF", .referenceIndex = 198, .category = Other},
    {.name = "X360_SCRIPT_RPF", .referenceIndex = 199, .category = Other},
    {.name = "PED_FIRST_PERSON_ALTERNATE_DATA", .referenceIndex = 200, .category = Peds},
});
} // namespace

const DataFileTypeInfo* FindDataFileType(std::string_view name)
{
    const std::string upper = util::ToUpper(util::Trim(name));
    const auto match =
        std::ranges::find(kDataFileTypes, std::string_view{upper}, &DataFileTypeInfo::name);
    return match != kDataFileTypes.end() ? &*match : nullptr;
}

std::span<const DataFileTypeInfo> GetDataFileTypes()
{
    return kDataFileTypes;
}

bool IsCategoryEnabled(DataFileCategory category, const config::DataFileSettings& settings)
{
    switch (category)
    {
    case Maps:
        return true;
    case Vehicles:
        return settings.vehicles;
    case Weapons:
        return settings.weapons;
    case Peds:
        return settings.peds;
    case Audio:
        return settings.audio;
    case Other:
        return settings.other;
    }
    return true;
}

std::string_view ToString(DataFileCategory category)
{
    switch (category)
    {
    case Maps:
        return "maps";
    case Vehicles:
        return "vehicles";
    case Weapons:
        return "weapons";
    case Peds:
        return "peds";
    case Audio:
        return "audio";
    case Other:
        return "other";
    }
    return "other";
}

std::string_view ToString(DataFilePolicy policy)
{
    switch (policy)
    {
    case Generic:
        return "generic";
    case TypeRequest:
        return "type request";
    case Refused:
        return "refused";
    case Deferred:
        return "deferred";
    case Packfile:
        return "packfile";
    }
    return "generic";
}
} // namespace spl::streaming
