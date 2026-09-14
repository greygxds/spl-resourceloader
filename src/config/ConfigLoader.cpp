#include "config/ConfigLoader.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "config/DefaultConfig.h"
#include "config/LoaderConfig.h"
#include "util/Strings.h"

namespace spl::config
{
namespace
{
constexpr std::string_view kTableLoader = "loader";
constexpr std::string_view kTablePaths = "paths";
constexpr std::string_view kTableResources = "resources";
constexpr std::string_view kTableMods = "mods";
constexpr std::string_view kTableLogging = "logging";
constexpr std::string_view kTableStreaming = "streaming";
constexpr std::string_view kTableDataFiles = "data_files";
constexpr std::string_view kTableMemory = "memory";
constexpr std::string_view kTableDiagnostics = "diagnostics";

struct LevelName
{
    std::string_view text;
    LogLevel level;
    bool isAlias;
};

// Canonical names first, so ToString() picks them; the aliases exist because "warn" and "err"
// are what people type out of habit.
constexpr std::array kLevelNames = {
    LevelName{"trace", LogLevel::Trace, false}, LevelName{"debug", LogLevel::Debug, false},
    LevelName{"info", LogLevel::Info, false},   LevelName{"warning", LogLevel::Warning, false},
    LevelName{"error", LogLevel::Error, false}, LevelName{"critical", LogLevel::Critical, false},
    LevelName{"off", LogLevel::Off, false},     LevelName{"warn", LogLevel::Warning, true},
    LevelName{"err", LogLevel::Error, true},
};

struct ParsedLevel
{
    LogLevel level;
    std::string_view canonicalName;
    bool usedAlias;
};

std::optional<ParsedLevel> ParseLogLevel(std::string_view text)
{
    const std::string lowered = util::ToLower(util::Trim(text));
    const auto match = std::ranges::find_if(kLevelNames, [&](const LevelName& candidate)
                                            { return candidate.text == lowered; });
    if (match == kLevelNames.end())
    {
        return std::nullopt;
    }
    return ParsedLevel{.level = match->level,
                       .canonicalName = ToString(match->level),
                       .usedAlias = match->isAlias};
}

std::optional<DuplicatePolicy> ParseDuplicatePolicy(std::string_view text)
{
    const std::string lowered = util::ToLower(util::Trim(text));
    if (lowered == "first")
    {
        return DuplicatePolicy::FirstWins;
    }
    if (lowered == "last")
    {
        return DuplicatePolicy::LastWins;
    }
    return std::nullopt;
}

std::optional<SafeMode> ParseSafeMode(std::string_view text)
{
    const std::string lowered = util::ToLower(util::Trim(text));
    if (lowered == "auto")
    {
        return SafeMode::Auto;
    }
    if (lowered == "off")
    {
        return SafeMode::Off;
    }
    return std::nullopt;
}

std::optional<MapReloadStrategy> ParseMapReloadStrategy(std::string_view text)
{
    const std::string lowered = util::ToLower(util::Trim(text));
    if (lowered == "auto")
    {
        return MapReloadStrategy::Auto;
    }
    if (lowered == "change_set_replay")
    {
        return MapReloadStrategy::ChangeSetReplay;
    }
    if (lowered == "content_group_toggle")
    {
        return MapReloadStrategy::ContentGroupToggle;
    }
    return std::nullopt;
}

/// Reads the keys of one TOML table, remembering which ones it recognized so that whatever is
/// left over can be reported as a typo. Every failure is a warning that keeps the default, so
/// one bad key never costs the user the rest of their configuration.
class TableReader
{
public:
    TableReader(const toml::table* table, std::string_view tableName,
                ConfigDiagnostics& diagnostics)
        : m_table(table), m_tableName(tableName), m_diagnostics(&diagnostics)
    {
    }

    void Read(std::string_view key, bool& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        if (const std::optional<bool> value = node->value<bool>())
        {
            target = *value;
            return;
        }
        WarnWrongType(key, "a boolean");
    }

    void Read(std::string_view key, std::string& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        target = util::Trim(*value);
    }

    void Read(std::string_view key, std::filesystem::path& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        const std::string trimmed = util::Trim(*value);
        if (trimmed.empty())
        {
            Warn(key, "is empty (ignored)");
            return;
        }
        target = std::filesystem::path{trimmed};
    }

    void Read(std::string_view key, uint32_t& target,
              uint32_t maximum = std::numeric_limits<uint32_t>::max())
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<int64_t> value = node->value<int64_t>();
        if (!value)
        {
            WarnWrongType(key, "an integer");
            return;
        }
        if (*value < 0 || *value > maximum)
        {
            Warn(key, maximum == std::numeric_limits<uint32_t>::max()
                          ? std::string{"is out of range (ignored)"}
                          : fmt::format("is out of range 0-{} (ignored)", maximum));
            return;
        }
        target = static_cast<uint32_t>(*value);
    }

    void ReadLevel(std::string_view key, LogLevel& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        const std::optional<ParsedLevel> level = ParseLogLevel(*value);
        if (!level)
        {
            Warn(key, fmt::format("has unknown level '{}' (using '{}')", util::Trim(*value),
                                  ToString(target)));
            return;
        }
        if (level->usedAlias)
        {
            Warn(key, fmt::format("uses the alias '{}'; prefer '{}'", util::Trim(*value),
                                  level->canonicalName));
        }
        target = level->level;
    }

    void ReadDuplicatePolicy(std::string_view key, DuplicatePolicy& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        const std::optional<DuplicatePolicy> policy = ParseDuplicatePolicy(*value);
        if (!policy)
        {
            Warn(key, fmt::format("has unknown policy '{}' (expected 'first' or 'last')",
                                  util::Trim(*value)));
            return;
        }
        target = *policy;
    }

    void ReadSafeMode(std::string_view key, SafeMode& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        // TOML has no enums, and "safe_mode = false" is what people will write for "off".
        if (const std::optional<bool> flag = node->value<bool>())
        {
            target = *flag ? SafeMode::Auto : SafeMode::Off;
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        const std::optional<SafeMode> mode = ParseSafeMode(*value);
        if (!mode)
        {
            Warn(key, fmt::format("has unknown mode '{}' (expected 'auto' or 'off')",
                                  util::Trim(*value)));
            return;
        }
        target = *mode;
    }

    void ReadMapReloadStrategy(std::string_view key, MapReloadStrategy& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const std::optional<std::string> value = node->value<std::string>();
        if (!value)
        {
            WarnWrongType(key, "a string");
            return;
        }
        const std::optional<MapReloadStrategy> strategy = ParseMapReloadStrategy(*value);
        if (!strategy)
        {
            Warn(key, fmt::format("has unknown strategy '{}' (expected 'auto', "
                                  "'change_set_replay' or 'content_group_toggle')",
                                  util::Trim(*value)));
            return;
        }
        target = *strategy;
    }

    /// A list of names (resources, data-file types): entries are trimmed, empties dropped, and
    /// duplicates removed case-insensitively, because names are matched that way everywhere.
    void ReadNameList(std::string_view key, std::vector<std::string>& target)
    {
        const toml::node* node = Find(key);
        if (node == nullptr)
        {
            return;
        }
        const toml::array* array = node->as_array();
        if (array == nullptr)
        {
            WarnWrongType(key, "an array of strings");
            return;
        }

        std::vector<std::string> names;
        for (const toml::node& element : *array)
        {
            const std::optional<std::string> value = element.value<std::string>();
            if (!value)
            {
                Warn(key, "contains a non-string entry (ignored)");
                continue;
            }
            const std::string trimmed = util::Trim(*value);
            if (trimmed.empty())
            {
                Warn(key, "contains an empty entry (ignored)");
                continue;
            }
            const bool duplicate =
                std::ranges::any_of(names, [&](const std::string& seen)
                                    { return util::EqualsIgnoreCase(seen, trimmed); });
            if (duplicate)
            {
                Warn(key, fmt::format("lists '{}' more than once (ignored)", trimmed));
                continue;
            }
            names.push_back(trimmed);
        }
        target = std::move(names);
    }

    /// Reports every key of the table that no Read() call claimed. Call it last.
    void ReportUnknownKeys()
    {
        if (m_table == nullptr)
        {
            return;
        }
        for (const auto& [key, value] : *m_table)
        {
            const std::string_view name{key.str()};
            const bool known = std::ranges::any_of(m_knownKeys, [&](const std::string& claimed)
                                                   { return claimed == name; });
            if (!known)
            {
                m_diagnostics->warnings.push_back(
                    fmt::format("Unknown key '{}' (ignored)", QualifiedName(name)));
            }
        }
    }

private:
    [[nodiscard]] std::string QualifiedName(std::string_view key) const
    {
        return fmt::format("{}.{}", m_tableName, key);
    }

    /// Marks key as recognized and returns its node, or nullptr when the table omits it.
    const toml::node* Find(std::string_view key)
    {
        m_knownKeys.emplace_back(key);
        if (m_table == nullptr)
        {
            return nullptr;
        }
        return m_table->get(key);
    }

    void Warn(std::string_view key, std::string_view detail)
    {
        m_diagnostics->warnings.push_back(fmt::format("'{}' {}", QualifiedName(key), detail));
    }

    void WarnWrongType(std::string_view key, std::string_view expected)
    {
        Warn(key, fmt::format("is not {} (using the default)", expected));
    }

    const toml::table* m_table;
    std::string m_tableName;
    ConfigDiagnostics* m_diagnostics;
    std::vector<std::string> m_knownKeys;
};

/// The sub-table of root named tableName, or nullptr. A key of the wrong shape
/// (`logging = 3`) is reported and treated as absent.
const toml::table* GetTable(const toml::table& root, std::string_view name,
                            ConfigDiagnostics& diagnostics)
{
    const toml::node* node = root.get(name);
    if (node == nullptr)
    {
        return nullptr;
    }
    const toml::table* table = node->as_table();
    if (table == nullptr)
    {
        diagnostics.warnings.push_back(fmt::format("'{}' is not a table (ignored)", name));
    }
    return table;
}

void ReportUnknownTables(const toml::table& root, ConfigDiagnostics& diagnostics)
{
    constexpr std::array kKnownTables = {kTableLoader,    kTablePaths,   kTableResources,
                                         kTableMods,      kTableLogging, kTableStreaming,
                                         kTableDataFiles, kTableMemory,  kTableDiagnostics};
    for (const auto& [key, value] : root)
    {
        const std::string_view name{key.str()};
        const bool known = std::ranges::any_of(kKnownTables, [&](std::string_view candidate)
                                               { return candidate == name; });
        if (!known)
        {
            diagnostics.warnings.push_back(fmt::format("Unknown table '{}' (ignored)", name));
        }
    }
}
} // namespace

std::string_view ToString(LogLevel level)
{
    const auto match =
        std::ranges::find_if(kLevelNames, [&](const LevelName& candidate)
                             { return !candidate.isAlias && candidate.level == level; });
    return match != kLevelNames.end() ? match->text : "warning";
}

ConfigLoadResult ConfigLoader::Parse(std::string_view tomlText)
{
    ConfigLoadResult result;

    toml::table root;
    try
    {
        root = toml::parse(tomlText);
    }
    catch (const toml::parse_error& error)
    {
        // The source region is what makes the message actionable, so it leads the line.
        const toml::source_position position = error.source().begin;
        result.diagnostics.errors.push_back(
            fmt::format("config.toml line {}, column {}: {} (using defaults)", position.line,
                        position.column, error.description()));
        return result; // config stays at its defaults
    }
    catch (const std::exception& error)
    {
        result.diagnostics.errors.push_back(
            fmt::format("config.toml could not be parsed: {} (using defaults)", error.what()));
        return result;
    }

    LoaderConfig& config = result.config;
    ConfigDiagnostics& diagnostics = result.diagnostics;

    {
        TableReader reader{GetTable(root, kTableLoader, diagnostics), kTableLoader, diagnostics};
        reader.Read("enabled", config.loader.enabled);
        reader.Read("console", config.loader.console);
        reader.ReadSafeMode("safe_mode", config.loader.safeMode);
        reader.Read("early_init", config.loader.earlyInit);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTablePaths, diagnostics), kTablePaths, diagnostics};
        reader.Read("resources", config.paths.resources);
        reader.Read("mods", config.paths.mods);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableResources, diagnostics), kTableResources,
                           diagnostics};
        reader.Read("enabled", config.resources.enabled);
        reader.ReadNameList("disabled", config.resources.disabled);
        reader.ReadNameList("priority", config.resources.priority);
        reader.Read("accept_legacy_manifest", config.resources.acceptLegacyManifest);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableMods, diagnostics), kTableMods, diagnostics};
        reader.Read("enabled", config.mods.enabled);
        reader.ReadNameList("disabled", config.mods.disabled);
        reader.ReadNameList("priority", config.mods.priority);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableLogging, diagnostics), kTableLogging, diagnostics};
        reader.ReadLevel("level", config.logging.level);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableStreaming, diagnostics), kTableStreaming,
                           diagnostics};
        reader.Read("enabled", config.streaming.enabled);
        reader.Read("load_textures", config.streaming.loadTextures);
        reader.Read("load_models", config.streaming.loadModels);
        reader.Read("load_maps", config.streaming.loadMaps);
        reader.Read("load_collisions", config.streaming.loadCollisions);
        reader.Read("load_manifests", config.streaming.loadManifests);
        reader.Read("load_animations", config.streaming.loadAnimations);
        reader.Read("allow_overrides", config.streaming.allowOverrides);
        reader.ReadDuplicatePolicy("duplicate_policy", config.streaming.duplicatePolicy);
        reader.Read("auto_request_ytyp", config.streaming.autoRequestYtyp);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableDataFiles, diagnostics), kTableDataFiles,
                           diagnostics};
        reader.Read("enabled", config.dataFiles.enabled);
        reader.Read("vehicles", config.dataFiles.vehicles);
        reader.Read("weapons", config.dataFiles.weapons);
        reader.Read("peds", config.dataFiles.peds);
        reader.Read("audio", config.dataFiles.audio);
        reader.Read("other", config.dataFiles.other);
        reader.ReadNameList("disabled_types", config.dataFiles.disabledTypes);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableMemory, diagnostics), kTableMemory, diagnostics};
        reader.Read("extended_texture_budget", config.memory.extendedTextureBudget);
        reader.Read("texture_budget_scale", config.memory.textureBudgetScale,
                    MemorySettings::kMaxTextureBudgetScale);
        reader.Read("extended_streaming_memory", config.memory.extendedStreamingMemory);
        reader.ReportUnknownKeys();
    }
    {
        TableReader reader{GetTable(root, kTableDiagnostics, diagnostics), kTableDiagnostics,
                           diagnostics};
        reader.Read("dump_streaming_modules", config.diagnostics.dumpStreamingModules);
        reader.Read("validate_rsc_headers", config.diagnostics.validateRscHeaders);
        reader.Read("asset_size_warning_mib", config.diagnostics.assetSizeWarningMiB);
        reader.Read("write_minidump", config.diagnostics.writeMinidump);
        reader.ReadMapReloadStrategy("map_reload_strategy", config.diagnostics.mapReloadStrategy);
        reader.ReportUnknownKeys();
    }

    ReportUnknownTables(root, diagnostics);
    return result;
}

ConfigLoadResult ConfigLoader::LoadOrCreate(const std::filesystem::path& file)
{
    ConfigLoadResult result;

    std::error_code error;
    if (!std::filesystem::exists(file, error))
    {
        result.wroteDefault = WriteDefaultConfig(file);
        if (!result.wroteDefault)
        {
            result.diagnostics.warnings.push_back(
                fmt::format("Could not write the default configuration to '{}' (using defaults)",
                            file.string()));
            return result;
        }
    }

    std::ifstream stream{file, std::ios::binary};
    if (!stream)
    {
        result.diagnostics.errors.push_back(
            fmt::format("Could not open '{}' (using defaults)", file.string()));
        return result;
    }

    std::ostringstream contents;
    contents << stream.rdbuf();

    const bool wroteDefault = result.wroteDefault;
    result = Parse(contents.str());
    result.wroteDefault = wroteDefault;
    return result;
}
} // namespace spl::config
