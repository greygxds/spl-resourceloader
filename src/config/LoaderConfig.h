#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spl::config
{
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

/// What happens when two resources contribute an asset with the same file name.
/// Resource load order decides which one is "first".
enum class DuplicatePolicy
{
    FirstWins,
    LastWins
};

/// What happens after a session that crashed while resources were being registered.
enum class SafeMode
{
    Auto, ///< skip the resource that was registering, or everything when that is unknown
    Off   ///< always register everything
};

struct LoaderSettings
{
    bool enabled = true;
    bool console = false; ///< live log window that also takes commands ("help")
    SafeMode safeMode = SafeMode::Auto;

    /// Run on a game build newer than every verified one when all its signatures resolve and
    /// every layout check passes. Map-store patches stay off on such a build regardless.
    bool allowUnverifiedBuilds = true;

    /// Start with the game rather than with story mode: mods replace game files
    /// before they are read, level metas load, and registration happens during the load.
    /// Without the hooks it needs, the loader still starts with story mode.
    bool earlyInit = true;

    bool operator==(const LoaderSettings&) const = default;
};

struct PathSettings
{
    /// Relative paths resolve against the data folder, <GTA V>/resourceLoader.
    std::filesystem::path resources = "resources";
    std::filesystem::path mods = "mods"; ///< user-installed .rpf mods

    bool operator==(const PathSettings&) const = default;
};

struct ResourceSettings
{
    bool autoDiscover = true;
    std::vector<std::string> disabled; ///< resource names, compared case-insensitively
    std::vector<std::string> priority; ///< loaded first, in this order; the rest alphabetically
    bool acceptLegacyManifest = true;  ///< __resource.lua as well as fxmanifest.lua

    bool operator==(const ResourceSettings&) const = default;
};

struct LoggingSettings
{
    LogLevel level = LogLevel::Warning;

    bool operator==(const LoggingSettings&) const = default;
};

/// User-installed .rpf mods: extracted to the cache dir and loaded as
/// resources sorted after everything discovered, so same-named files lose to resources.
struct ModsSettings
{
    bool enabled = true;
    std::vector<std::string> disabled; ///< .rpf file stems, compared case-insensitively
    std::vector<std::string> priority; ///< loaded first among mods, in this order

    bool operator==(const ModsSettings&) const = default;
};

struct StreamingSettings
{
    bool enabled = true;
    bool loadTextures = true;   ///< .ytd
    bool loadModels = true;     ///< .ydr .ydd .yft .ymt
    bool loadMaps = true;       ///< .ymap .ytyp .ynv .ynd
    bool loadCollisions = true; ///< .ybn
    bool loadManifests = true;  ///< .ymf
    bool loadAnimations = true; ///< .ycd
    bool allowOverrides = true; ///< let a resource replace a same-named game asset
    DuplicatePolicy duplicatePolicy = DuplicatePolicy::FirstWins;

    /// Load every streamed .ytyp as if the manifest had a DLC_ITYP_REQUEST for it. Not FiveM
    /// behaviour, which is why it is off.
    bool autoRequestYtyp = false;

    bool operator==(const StreamingSettings&) const = default;
};

/// Which manifest `data_file` entries are handed to the game (FiveM parity: any type the game
/// has a mounter for).
struct DataFileSettings
{
    bool enabled = true;
    bool vehicles = true; ///< handling, vehicles, carcols, carvariations, layouts, ...
    bool weapons = true;  ///< weapon info, components, archetypes, animations, pickups, ...
    bool peds = true;     ///< ped metadata, personalities, shop apparel, ...
    bool audio = true;    ///< AUDIO_* (game data, sounds, wave packs, ...)
    bool other = true;    ///< every other type the game knows
    std::vector<std::string> disabledTypes; ///< type names, compared case-insensitively

    bool operator==(const DataFileSettings&) const = default;
};

/// How the map data store is rebuilt after maps were registered.
enum class MapReloadStrategy
{
    Auto,              ///< the change-set replay when its patch sites are intact, else the toggle
    ChangeSetReplay,   ///< the same as Auto; kept so configs that name it keep working
    ContentGroupToggle ///< never patch LoadChangeSet (new .ymap files may not stream in)
};

struct DiagnosticsSettings
{
    bool dumpStreamingModules = false; ///< log streaming modules and data-file mounters
    bool validateRscHeaders = true;
    uint32_t assetSizeWarningMiB = 256;

    /// Write resourceLoader/crash.dmp next to crash.txt when the loader crashes the game.
    bool writeMinidump = false;

    /// Advanced, so the default config file does not list it.
    MapReloadStrategy mapReloadStrategy = MapReloadStrategy::Auto;

    bool operator==(const DiagnosticsSettings&) const = default;
};

struct LoaderConfig
{
    LoaderSettings loader;
    PathSettings paths;
    ResourceSettings resources;
    ModsSettings mods;
    LoggingSettings logging;
    StreamingSettings streaming;
    DataFileSettings dataFiles;
    DiagnosticsSettings diagnostics;

    bool operator==(const LoaderConfig&) const = default;
};
} // namespace spl::config
