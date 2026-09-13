#include "config/DefaultConfig.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace spl::config
{
namespace
{
// Keep this in step with LoaderConfig's defaults. A unit test parses this text and compares
// the result with a default-constructed LoaderConfig, so drift between the two fails the build.
constexpr std::string_view kDefaultConfig =
    R"(# Relative paths are relative to this folder (GTA V/resourceLoader).

[loader]
enabled = true
console = false                 # live log window that takes commands; type "help" in it
safe_mode = "auto"              # after a crash during registration: "auto" skips the culprit, "off"
allow_unverified_builds = true  # run on a newer game build when every signature still resolves
early_init = true               # start with the game, as FiveM does; false waits for story mode

[paths]
resources = "resources"
mods = "mods"                     # user-installed .rpf mods (FiveM mods/ folder equivalent)

[resources]
auto_discover = true
disabled = []                   # folder names, case-insensitive
priority = []                   # loaded first, in this order; the rest alphabetically
accept_legacy_manifest = true   # __resource.lua

[mods]
enabled = true                  # extract mods/*.rpf and load them after resources
disabled = []                   # .rpf names without the extension, case-insensitive
priority = []                   # loaded first among mods, in this order; the rest alphabetically

[logging]
level = "warning"               # trace, debug, info, warning, error, critical, off

[streaming]
enabled = true
load_textures = true            # .ytd
load_models = true              # .ydr .ydd .yft .ymt
load_maps = true                # .ymap .ytyp .ynv .ynd
load_collisions = true          # .ybn
load_manifests = true           # .ymf
load_animations = true          # .ycd
allow_overrides = true          # let resources replace original game assets
duplicate_policy = "first"      # same file in two resources: "first" or "last" by load order wins
auto_request_ytyp = false       # load every streamed .ytyp even without DLC_ITYP_REQUEST

[data_files]                    # manifest data_file entries, as FiveM loads them
enabled = true
vehicles = true                 # handling, vehicles, carcols, carvariations, vehicle layouts
weapons = true                  # weapon info, components, archetypes, animations, pickups
peds = true                     # ped metadata, personalities, shop apparel
audio = true                    # AUDIO_GAMEDATA, AUDIO_SOUNDDATA, AUDIO_WAVEPACK, ...
other = true                    # every other type the game knows
disabled_types = []             # type names, e.g. ["CARCOLS_FILE"]

[diagnostics]
dump_streaming_modules = false  # log the game's streaming modules and data-file mounters
validate_rsc_headers = true
asset_size_warning_mib = 256    # warn when an asset needs more memory than this
write_minidump = false          # crash.dmp next to crash.txt when the loader crashes the game
)";
} // namespace

std::string_view DefaultConfigText()
{
    return kDefaultConfig;
}

bool WriteDefaultConfig(const std::filesystem::path& file)
{
    if (std::filesystem::exists(file))
    {
        return false; // the user's file is never overwritten, not even a broken one
    }

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
    {
        return false;
    }

    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    if (!stream)
    {
        return false;
    }
    stream << kDefaultConfig;
    return stream.good();
}
} // namespace spl::config
