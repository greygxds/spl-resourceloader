#include "config/DefaultConfig.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "util/Strings.h"

namespace spl::config
{
namespace
{
/// One table of the default text, with each key's whole line so its comment comes along.
struct DefaultTable
{
    std::string name;
    std::string headerLine;
    std::vector<std::pair<std::string, std::string>> keyLines; ///< key, line
};

/// The text split at '\n', each line without its terminator.
struct Lines
{
    std::vector<std::string> lines;
    std::string newline = "\n";   ///< "\r\n" when the file uses it
    bool endsWithNewline = false; ///< the last line was terminated
};

[[nodiscard]] Lines SplitLines(std::string_view text)
{
    Lines split;
    if (text.find("\r\n") != std::string_view::npos)
    {
        split.newline = "\r\n";
    }
    split.endsWithNewline = !text.empty() && text.back() == '\n';

    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = text.find('\n', start);
        std::string_view line = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        split.lines.emplace_back(line);
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    return split;
}

/// The default text is ours and keeps every table header and key on a line of its own, so a
/// line scan is enough to find them.
[[nodiscard]] std::vector<DefaultTable> ScanDefaultTables(std::string_view defaultText)
{
    std::vector<DefaultTable> tables;
    for (const std::string& line : SplitLines(defaultText).lines)
    {
        const std::string trimmed = util::Trim(line);
        if (trimmed.empty() || trimmed.front() == '#')
        {
            continue;
        }
        if (trimmed.front() == '[')
        {
            const std::size_t close = trimmed.find(']');
            tables.push_back(
                DefaultTable{.name = util::Trim(trimmed.substr(1, close - 1)), .headerLine = line});
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (tables.empty() || equals == std::string::npos)
        {
            continue;
        }
        tables.back().keyLines.emplace_back(util::Trim(trimmed.substr(0, equals)), line);
    }
    return tables;
}

[[nodiscard]] std::optional<toml::table> TryParse(std::string_view text)
{
    try
    {
        return toml::parse(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

/// The 1-based line a key missing from table goes after: the end of its last value, or the
/// header when it has none. A sub-table's lines belong under its own header, so they do not count.
[[nodiscard]] std::size_t LastLineOf(const toml::table& table)
{
    std::size_t last = table.source().begin.line;
    for (const auto& [key, value] : table)
    {
        const toml::table* const child = value.as_table();
        if (child != nullptr && !child->is_inline())
        {
            continue;
        }
        last = std::max<std::size_t>(last, value.source().end.line);
    }
    return last;
}

// Keep this in step with LoaderConfig's defaults. A unit test parses this text and compares
// the result with a default-constructed LoaderConfig, so drift between the two fails the build.
constexpr std::string_view kDefaultConfig =
    R"(# Relative paths are relative to this folder (GTA V/resourceLoader).

[loader]
enabled = true
console = false                 # live log window that takes commands; type "help" in it
safe_mode = "auto"              # after a crash during registration: "auto" skips the culprit, "off"
early_init = true               # start with the game, as FiveM does; false waits for story mode

[paths]
resources = "resources"
mods = "mods"                     # user-installed .rpf mods (FiveM mods/ folder equivalent)

[resources]
enabled = true                  # load the resources folder
disabled = []                   # folder names, case-insensitive
priority = []                   # loaded first, in this order; the rest alphabetically
accept_legacy_manifest = true   # __resource.lua

[mods]
enabled = true                  # load mods/*.rpf after resources
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
mp_maps = true                  # start with GTA Online's map layer, as FiveM does; needs early_init
deferred = ["hei_*", "apa_*", "lr_*", "vw_*", "bkr_*"] # MP-layer maps wait for the game's slot

[data_files]                    # manifest data_file entries, as FiveM loads them
enabled = true
vehicles = true                 # handling, vehicles, carcols, carvariations, vehicle layouts
weapons = true                  # weapon info, components, archetypes, animations, pickups
peds = true                     # ped metadata, personalities, shop apparel
audio = true                    # AUDIO_GAMEDATA, AUDIO_SOUNDDATA, AUDIO_WAVEPACK, ...
other = true                    # every other type the game knows
disabled_types = []             # type names, e.g. ["CARCOLS_FILE"]

[memory]                        # memory extensions; they need early_init = true
extended_texture_budget = false # a 3 GB texture VRAM budget, instead of the game's own
texture_budget_scale = 0        # 0-12: that budget times 1.0 to 2.0
extended_streaming_memory = false # 12 GB+ RAM: bigger streaming allocator and resource cache

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

std::optional<ConfigUpgrade> AddMissingOptions(std::string_view userText,
                                               std::string_view defaultText)
{
    const std::optional<toml::table> user = TryParse(userText);
    if (!user || !TryParse(defaultText))
    {
        return std::nullopt;
    }

    ConfigUpgrade upgrade;
    std::map<std::size_t, std::vector<std::string>> insertAfterLine; ///< 1-based line, lines
    std::vector<std::string> appended;

    for (const DefaultTable& defaults : ScanDefaultTables(defaultText))
    {
        const toml::node* const node = user->get(defaults.name);
        if (node == nullptr)
        {
            if (!appended.empty())
            {
                appended.emplace_back();
            }
            appended.push_back(defaults.headerLine);
            for (const auto& [key, line] : defaults.keyLines)
            {
                appended.push_back(line);
                upgrade.addedOptions.push_back(defaults.name + "." + key);
            }
            continue;
        }

        const toml::table* const table = node->as_table();
        if (table == nullptr || table->is_inline() || table->source().begin.line == 0)
        {
            continue; // not something lines can be added to; the loader warns about it
        }
        for (const auto& [key, line] : defaults.keyLines)
        {
            if (table->contains(key))
            {
                continue;
            }
            insertAfterLine[LastLineOf(*table)].push_back(line);
            upgrade.addedOptions.push_back(defaults.name + "." + key);
        }
    }

    if (upgrade.addedOptions.empty())
    {
        upgrade.text = std::string{userText};
        return upgrade;
    }

    const Lines split = SplitLines(userText);
    std::vector<std::string> lines;
    lines.reserve(split.lines.size() + upgrade.addedOptions.size() + appended.size() + 1);
    for (std::size_t index = 0; index < split.lines.size(); ++index)
    {
        lines.push_back(split.lines[index]);
        if (const auto inserts = insertAfterLine.find(index + 1); inserts != insertAfterLine.end())
        {
            lines.insert(lines.end(), inserts->second.begin(), inserts->second.end());
        }
    }
    if (!appended.empty())
    {
        if (!lines.empty() && !util::Trim(lines.back()).empty())
        {
            lines.emplace_back(); // a blank line before the new tables, as between the others
        }
        lines.insert(lines.end(), appended.begin(), appended.end());
    }

    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        upgrade.text += lines[index];
        if (index + 1 < lines.size() || split.endsWithNewline || !appended.empty())
        {
            upgrade.text += split.newline;
        }
    }

    // Belt and braces: a result that does not parse is never handed back to be written.
    if (!TryParse(upgrade.text))
    {
        return std::nullopt;
    }
    return upgrade;
}
} // namespace spl::config
