#include "mods/ModLayout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "core/Result.h"
#include "mods/AssemblyParser.h"
#include "mods/ModFileTree.h"
#include "mods/ModOverlay.h"
#include "mods/ModsScanner.h"
#include "rpf/RpfReader.h"
#include "util/Strings.h"

namespace spl::mods
{
namespace
{
/// Basename → data-file type for loose mod metas. Exact conventional names only: a wrong
/// type loads the file into the wrong game system, so anything unrecognized is skipped
/// with a warning instead of guessed.
struct MetaTypeName
{
    std::string_view file;
    std::string_view type;
};

constexpr std::array kMetaTypes = {
    MetaTypeName{"handling.meta", "HANDLING_FILE"},
    MetaTypeName{"vehicles.meta", "VEHICLE_METADATA_FILE"},
    MetaTypeName{"carcols.meta", "CARCOLS_FILE"},
    MetaTypeName{"carvariations.meta", "VEHICLE_VARIATION_FILE"},
    MetaTypeName{"vehiclelayouts.meta", "VEHICLE_LAYOUTS_FILE"},
    MetaTypeName{"vehicleextras.dat", "VEHICLEEXTRAS_FILE"},
    MetaTypeName{"weaponarchetypes.meta", "WEAPON_METADATA_FILE"},
    MetaTypeName{"weapons.meta", "WEAPONINFO_FILE"},
    MetaTypeName{"weaponcomponents.meta", "WEAPONCOMPONENTSINFO_FILE"},
    MetaTypeName{"weaponanimations.meta", "WEAPON_ANIMATIONS_FILE"},
    MetaTypeName{"peds.meta", "PED_METADATA_FILE"},
    MetaTypeName{"pedpersonality.meta", "PED_PERSONALITY_FILE"},
    MetaTypeName{"gtxd.meta", "GTXD_PARENTING_DATA"},
};

[[nodiscard]] std::optional<std::string_view> InferDataFileType(std::string_view target)
{
    const std::size_t slash = target.find_last_of('/');
    const std::string base =
        util::ToLower(slash == std::string_view::npos ? target : target.substr(slash + 1));
    for (const MetaTypeName& known : kMetaTypes)
    {
        if (base == known.file)
        {
            return known.type;
        }
    }
    if (base.starts_with("timecycle_mods_") && base.ends_with(".xml"))
    {
        return "TIMECYCLEMOD_FILE";
    }
    return std::nullopt;
}

/// A game folder a mod's targets can replace files in, and the mount points serving it.
struct OverlayFolder
{
    std::string_view prefix;
    std::array<std::string_view, 2> mountPoints;
};

constexpr std::array kOverlayFolders = {
    OverlayFolder{.prefix = "common/", .mountPoints = {"common:/", "commoncrc:/"}},
    OverlayFolder{.prefix = "platform/", .mountPoints = {"platform:/", "platformcrc:/"}},
};

[[nodiscard]] std::optional<OverlayFolder> FindOverlayFolder(std::string_view target)
{
    for (const OverlayFolder& folder : kOverlayFolders)
    {
        if (target.size() > folder.prefix.size() && target.starts_with(folder.prefix))
        {
            return folder;
        }
    }
    return std::nullopt;
}

/// Records a folder's mounts the first time a file lands in it; that file is the probe.
void AddOverlayRoot(ModLayout::Result& result, const OverlayFolder& folder, std::string_view target)
{
    const std::filesystem::path path =
        result.root / folder.prefix.substr(0, folder.prefix.size() - 1);
    if (std::ranges::any_of(result.overlays, [&path](const ModLayout::OverlayRoot& root)
                            { return root.folder == path; }))
    {
        return;
    }
    for (const std::string_view mountPoint : folder.mountPoints)
    {
        result.overlays.push_back(
            ModLayout::OverlayRoot{.folder = path,
                                   .mountPoint = std::string{mountPoint},
                                   .probeFile = std::string{target.substr(folder.prefix.size())}});
    }
}

[[nodiscard]] std::string BaseName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

/// Archive-relative path of an assembly source: every source lives under the archive's
/// content/ folder (FiveM ModVFSDevice.cpp:266), whatever slashes it was written with.
[[nodiscard]] std::string ResolveSource(std::string_view source)
{
    std::string normalized{source};
    std::ranges::replace(normalized, '\\', '/');
    const std::size_t stripped = normalized.find_first_not_of('/');
    const std::string relative = (stripped == std::string::npos) ? "" : normalized.substr(stripped);
    return "content/" + relative;
}

/// Mod folder names come from archive stems, which are user input, and end up in VFS paths.
[[nodiscard]] std::string SanitizeName(std::string_view name)
{
    std::string out{name};
    for (char& c : out)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!plain)
        {
            c = '_';
        }
    }
    return out.empty() ? std::string{"mod"} : out;
}

[[nodiscard]] std::string EscapeLua(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\'' || c == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

/// FiveM's requiredVersion gate (ModVFSDevice.cpp:505-530): "" loads everywhere,
/// "min" needs that build or newer, "min-max" a closed range. An unknown game build
/// loads only unversioned DLCs.
[[nodiscard]] bool VersionAllows(std::string_view requiredVersion, uint32_t gameBuild)
{
    if (requiredVersion.empty())
    {
        return true;
    }
    if (gameBuild == 0)
    {
        return false;
    }
    const auto parseNumber = [](std::string_view text, uint32_t& number)
    {
        if (text.empty())
        {
            return false;
        }
        uint64_t value = 0;
        for (const char c : text)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
            value = value * 10 + static_cast<uint64_t>(c - '0');
            if (value > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
        }
        number = static_cast<uint32_t>(value);
        return true;
    };

    const std::size_t dash = requiredVersion.find('-');
    uint32_t minimum = 0;
    if (!parseNumber(requiredVersion.substr(0, dash), minimum) || gameBuild < minimum)
    {
        return false;
    }
    if (dash == std::string_view::npos)
    {
        return true;
    }
    uint32_t maximum = 0;
    return parseNumber(requiredVersion.substr(dash + 1), maximum) && gameBuild <= maximum;
}

/// Collects a mod's files as references into its archives, keeping every reader they point into.
struct LayoutBuilder
{
    ModLayout::Result& result;
    std::string archiveName;
    std::vector<std::unique_ptr<rpf::RpfReader>> archives;
    std::vector<ModFile> files;
    std::unordered_set<std::string> streamNames;
    std::size_t packCount = 0;

    /// Reads an archive-relative file (setup2.xml, content.xml, ...) as text.
    static std::optional<std::string> ReadText(const rpf::RpfReader& archive, std::string_view path)
    {
        const spl::Result<std::vector<std::byte>> bytes = archive.ReadFile(path);
        if (!bytes)
        {
            return std::nullopt;
        }
        const std::vector<std::byte>& value = bytes.GetValue();
        return std::string{reinterpret_cast<const char*>(value.data()), value.size()};
    }

    /// Moves reader where files can point into it for as long as the layout lives.
    const rpf::RpfReader& Keep(rpf::RpfReader reader)
    {
        return *archives.emplace_back(std::make_unique<rpf::RpfReader>(std::move(reader)));
    }

    /// Lays out the file at entryPath in archive as path. False, with a warning, when the
    /// archive cannot serve it.
    bool AddFile(std::string path, const rpf::RpfReader& archive, std::string_view entryPath)
    {
        if (const spl::Result<rpf::RpfReader::FileInfo> stat = archive.Stat(entryPath); !stat)
        {
            result.warnings.push_back("'" + archiveName + "': cannot read '" +
                                      std::string{entryPath} + "' (" + stat.GetMessage() + ")");
            return false;
        }
        files.push_back(
            ModFile{.path = std::move(path),
                    .source = ArchiveEntry{.archive = &archive, .path = std::string{entryPath}}});
        return true;
    }

    /// The top-level files of a nested archive, which FiveM registers under the archive's own tag
    /// (ModVFSDevice.cpp:286-352). A name an earlier pack of the mod already provided is left to
    /// that pack, except a .ymf: every pack has its own _manifest.ymf, so each gets a folder.
    void AddPack(const rpf::RpfReader& pack)
    {
        ++packCount;
        for (const rpf::RpfReader::EntryInfo& node : pack.Enumerate())
        {
            if (node.isDirectory || node.path.find('/') != std::string::npos)
            {
                continue; // top-level files only (FiveM MountFauxStreamingRpf)
            }
            const std::string name = util::ToLower(node.path);
            const bool manifest = name.ends_with(".ymf");
            if (!manifest && !streamNames.insert(name).second)
            {
                continue;
            }
            const std::string folder =
                manifest ? fmt::format("stream/pack{}/", packCount) : std::string{"stream/"};
            AddFile(folder + node.path, pack, node.path);
        }
    }

    /// stream/<basename>, first source wins so two targets never overwrite each other.
    /// source is archive-relative already (callers prepend content/ for assembly sources).
    void AddStreamed(const rpf::RpfReader& archive, std::string_view source,
                     std::string_view target)
    {
        const std::string name = BaseName(target);
        if (!streamNames.insert(name).second)
        {
            result.warnings.push_back("'" + archiveName + "': '" + std::string{source} +
                                      "' skipped, '" + name + "' is already provided");
            return;
        }
        AddFile("stream/" + name, archive, source);
    }
};

/// Opens a nested archive and keeps it, warning on behalf of mod when it fails.
[[nodiscard]] const rpf::RpfReader* OpenNested(LayoutBuilder& builder, std::string_view modName,
                                               const rpf::RpfReader& archive,
                                               std::string_view source)
{
    spl::Result<rpf::RpfReader> pack = archive.OpenNested(source);
    if (!pack)
    {
        builder.result.warnings.push_back("'" + std::string{modName} + "': cannot open nested '" +
                                          std::string{source} + "' (" + pack.GetMessage() + ")");
        return nullptr;
    }
    return &builder.Keep(std::move(pack.GetValue()));
}

/// Strips a "<device>:/" prefix off a content.xml filename, so "dlc_X:/x64/a.rpf" with
/// device "dlc_X" resolves inside the DLC archive. Anything else resolves relative too:
/// content DLCs are self-contained, and game-absolute paths cannot be served offline.
[[nodiscard]] std::string ResolveDlcPath(std::string_view device, std::string_view filename)
{
    std::string path{filename};
    std::ranges::replace(path, '\\', '/');
    const std::string prefix = util::ToLower(device) + ":/";
    if (util::ToLower(path).starts_with(prefix))
    {
        path.erase(0, prefix.size());
    }
    const std::size_t stripped = path.find_first_not_of('/');
    return stripped == std::string::npos ? std::string{} : path.substr(stripped);
}

/// The type request content.xml spells with the platform-neutral .ityp extension; the game
/// resolves it by streaming name to the platform's .ytyp.
constexpr std::string_view kItypRequestType = "DLC_ITYP_REQUEST";

[[nodiscard]] bool IsStreamed(const std::unordered_set<std::string>& streamNames,
                              std::string_view name)
{
    return std::ranges::any_of(streamNames, [name](const std::string& streamed)
                               { return util::EqualsIgnoreCase(streamed, name); });
}

void AddDlcs(const DiscoveredMod& mod, const rpf::RpfReader& archive, const ModOverlay& overlay,
             uint32_t gameBuild, LayoutBuilder& builder, std::string& manifest)
{
    struct PendingDlc
    {
        DlcDescriptor descriptor;
        std::string device; ///< lower-case deviceName
        std::string source; ///< lower-case, so dlc.rpf sorts before dlc1.rpf
        const rpf::RpfReader* pack = nullptr;
    };
    std::vector<PendingDlc> pending;
    for (const ModOverlay::DlcJob& job : overlay.DlcJobs())
    {
        const rpf::RpfReader* const pack =
            OpenNested(builder, mod.name, archive, ResolveSource(job.source));
        if (pack == nullptr)
        {
            continue;
        }
        const std::optional<std::string> setup = LayoutBuilder::ReadText(*pack, "setup2.xml");
        if (!setup)
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                              "' has no setup2.xml, skipped");
            continue;
        }
        const Setup2Result parsed = AssemblyParser::ParseSetup2(*setup);
        if (parsed.fatal)
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                              "' has a broken setup2.xml, skipped");
            continue;
        }
        if (parsed.descriptor.deviceName.empty())
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                              "' names no device, skipped");
            continue;
        }
        if (!VersionAllows(parsed.descriptor.requiredVersion, gameBuild))
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                              "' needs game build " +
                                              parsed.descriptor.requiredVersion + ", skipped");
            continue;
        }
        pending.push_back(PendingDlc{.descriptor = parsed.descriptor,
                                     .device = util::ToLower(parsed.descriptor.deviceName),
                                     .source = util::ToLower(job.source),
                                     .pack = pack});
    }
    std::ranges::stable_sort(pending,
                             [](const PendingDlc& left, const PendingDlc& right)
                             {
                                 return std::tie(left.descriptor.order, left.source) <
                                        std::tie(right.descriptor.order, right.source);
                             });

    struct PendingItem
    {
        const PendingDlc* dlc;
        ContentItem item;
        std::string relative; ///< device-relative, %PLATFORM% substituted
    };
    std::vector<PendingItem> items;
    for (const PendingDlc& dlc : pending)
    {
        const std::optional<std::string> content =
            LayoutBuilder::ReadText(*dlc.pack, "content.xml");
        if (!content)
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + dlc.descriptor.deviceName +
                                              "' has no content.xml, skipped");
            continue;
        }
        const ContentResult parsed = AssemblyParser::ParseContent(*content);
        if (parsed.fatal)
        {
            builder.result.warnings.push_back("'" + mod.name + "': '" + dlc.descriptor.deviceName +
                                              "' has a broken content.xml, skipped");
            continue;
        }
        for (const ContentItem& item : parsed.items)
        {
            std::string filename = item.filename;
            const std::size_t platform = filename.find("%PLATFORM%");
            if (platform != std::string::npos)
            {
                filename.replace(platform, std::string_view{"%PLATFORM%"}.size(), "x64");
            }
            std::string relative = ResolveDlcPath(dlc.descriptor.deviceName, filename);
            if (relative.empty())
            {
                builder.result.warnings.push_back("'" + mod.name + "': '" + item.filename +
                                                  "' names no file, skipped");
                continue;
            }
            items.push_back(
                PendingItem{.dlc = &dlc, .item = item, .relative = std::move(relative)});
        }
    }

    // The game mounts dlc.rpf, dlc1.rpf, ... of one pack under the same device, and each of their
    // content.xml files may list the others' files: a path resolves in any pack of its device.
    const auto findPack = [&pending](const std::string& device,
                                     std::string_view relative) -> const rpf::RpfReader*
    {
        for (const PendingDlc& candidate : pending)
        {
            if (candidate.device == device && candidate.pack->Contains(relative))
            {
                return candidate.pack;
            }
        }
        return nullptr;
    };
    std::unordered_set<std::string> handled; ///< "<device>:/<lower-case path>"
    const auto firstTime = [&handled](const PendingItem& entry)
    { return handled.insert(entry.dlc->device + ":/" + util::ToLower(entry.relative)).second; };
    const auto provide = [&builder](const PendingItem& entry)
    {
        builder.result.providedDlcFiles.push_back(
            ModLayout::DlcFile{.device = entry.dlc->device, .path = util::ToLower(entry.relative)});
    };
    const auto leaveUnresolved = [&builder, &mod](const PendingItem& entry)
    {
        // Reported only when no other mod provides it either (ReportUnresolvedDlcFiles).
        builder.result.unresolvedDlcFiles.push_back(ModLayout::DlcFile{
            .device = entry.dlc->device,
            .path = util::ToLower(entry.relative),
            .warning = "'" + mod.name + "': '" + entry.item.filename + "' ('" + entry.relative +
                       "') is in no pack of '" + entry.dlc->descriptor.deviceName + "', skipped"});
    };

    // Packfiles first, so the .ytyp files the type requests name are in stream/ by then; the game
    // too mounts a DLC's packfiles before it loads its data files.
    for (const PendingItem& entry : items)
    {
        if (entry.item.fileType != "RPF_FILE" || !firstTime(entry))
        {
            continue;
        }
        const rpf::RpfReader* const owner = findPack(entry.dlc->device, entry.relative);
        if (owner == nullptr)
        {
            leaveUnresolved(entry);
            continue;
        }
        provide(entry);
        if (const rpf::RpfReader* const pack =
                OpenNested(builder, mod.name, *owner, entry.relative))
        {
            builder.AddPack(*pack);
        }
    }

    for (const PendingItem& entry : items)
    {
        if (entry.item.fileType == "RPF_FILE" || !firstTime(entry))
        {
            continue;
        }
        if (entry.item.fileType == kItypRequestType)
        {
            std::string name = util::ToLower(BaseName(entry.relative));
            if (name.ends_with(".ityp"))
            {
                name.replace(name.size() - 5, 5, ".ytyp");
            }
            if (!IsStreamed(builder.streamNames, name))
            {
                // A loose .ytyp in the pack itself streams like any other.
                std::string loose = entry.relative;
                loose.replace(loose.size() - name.size(), name.size(), name);
                if (const rpf::RpfReader* const owner = findPack(entry.dlc->device, loose);
                    owner != nullptr && builder.AddFile("stream/" + name, *owner, loose))
                {
                    builder.streamNames.insert(name);
                }
            }
            provide(entry);
            // Requested even when no pack has it: it may name a .ytyp of the game, of another
            // DLC or of another mod, which the streaming plan checks and reports.
            const std::string target =
                IsStreamed(builder.streamNames, name) ? "stream/" + name : name;
            manifest +=
                "data_file '" + std::string{kItypRequestType} + "' '" + EscapeLua(target) + "'\n";
            continue;
        }

        const rpf::RpfReader* const owner = findPack(entry.dlc->device, entry.relative);
        if (owner == nullptr)
        {
            leaveUnresolved(entry);
            continue;
        }
        const std::string target =
            "__dlc__/" + entry.dlc->descriptor.deviceName + "/" + entry.relative;
        if (!builder.AddFile(target, *owner, entry.relative))
        {
            continue;
        }
        provide(entry);
        manifest += "data_file '" + entry.item.fileType + "' '" + EscapeLua(target) + "'\n";
    }
}

/// The FILETIME of the archive, which the game is given as every file's time.
[[nodiscard]] uint64_t GetArchiveFileTime(const std::filesystem::path& archive)
{
    std::error_code error;
    const std::filesystem::file_time_type written =
        std::filesystem::last_write_time(archive, error);
    return error ? 0 : static_cast<uint64_t>(written.time_since_epoch().count());
}
} // namespace

ModLayout::Result ModLayout::Build(const DiscoveredMod& mod, const std::filesystem::path& modsRoot,
                                   uint32_t gameBuild)
{
    Result result{.root = modsRoot / SanitizeName(mod.name)};
    LayoutBuilder builder{.result = result, .archiveName = mod.name};
    const auto finish = [&]
    {
        result.files = std::make_shared<const ModFileTree>(result.root, std::move(builder.archives),
                                                           std::move(builder.files),
                                                           GetArchiveFileTime(mod.absolutePath));
    };

    spl::Result<rpf::RpfReader> opened = rpf::RpfReader::Open(mod.absolutePath);
    if (!opened)
    {
        result.warnings.push_back("cannot open '" + util::ToUtf8(mod.absolutePath) + "' (" +
                                  opened.GetMessage() + ")");
        finish();
        return result;
    }
    const rpf::RpfReader& archive = builder.Keep(std::move(opened.GetValue()));
    const ModOverlay overlay = ModOverlay::Build(mod.package);

    for (const ModOverlay::StreamCandidate& candidate : overlay.StreamCandidates())
    {
        builder.AddStreamed(archive, ResolveSource(candidate.source), candidate.target);
    }
    for (const ModOverlay::Mapping& map : overlay.MapTargets())
    {
        builder.AddStreamed(archive, ResolveSource(map.source), map.target);
    }
    for (const ModOverlay::Mapping& manifest : overlay.ManifestTargets())
    {
        builder.AddStreamed(archive, ResolveSource(manifest.source), manifest.target);
    }

    for (const ModOverlay::FauxPackJob& job : overlay.FauxPackJobs())
    {
        if (const rpf::RpfReader* const pack =
                OpenNested(builder, mod.name, archive, ResolveSource(job.source)))
        {
            builder.AddPack(*pack);
        }
    }

    // Every game file the mod replaces, served through the overlay mounts. FiveM never mounts a
    // target outside common/ and platform/ either.
    std::unordered_set<std::string> overlayTargets;
    for (const ModOverlay::Mapping& mapping : overlay.Mappings())
    {
        const std::optional<OverlayFolder> folder = FindOverlayFolder(mapping.target);
        if (!folder || overlayTargets.contains(util::ToLower(mapping.target)) ||
            !builder.AddFile(mapping.target, archive, ResolveSource(mapping.source)))
        {
            continue;
        }
        overlayTargets.insert(util::ToLower(mapping.target));
        AddOverlayRoot(result, *folder, mapping.target);
    }

    // The game has read its startup metas before the loader runs, so the ones whose type is
    // known are loaded as data files as well.
    std::string manifest = "-- Generated from '" +
                           EscapeLua(util::ToUtf8(mod.absolutePath.filename())) +
                           "'; do not edit.\ngame 'gta5'\n";
    for (const ModOverlay::MetaFile& meta : overlay.MetaFiles())
    {
        const std::optional<std::string_view> type = InferDataFileType(meta.target);
        if (!type || !overlayTargets.contains(util::ToLower(meta.target)))
        {
            continue;
        }
        manifest += "data_file '" + std::string{*type} + "' '" + EscapeLua(meta.target) + "'\n";
    }
    AddDlcs(mod, archive, overlay, gameBuild, builder, manifest);

    builder.files.push_back(ModFile{.path = "fxmanifest.lua", .source = std::move(manifest)});
    finish();
    return result;
}

std::vector<std::string> ModLayout::ReportUnresolvedDlcFiles(std::span<const Result> results)
{
    std::unordered_set<std::string> provided; ///< "<device>:/<path>"
    for (const Result& result : results)
    {
        for (const DlcFile& file : result.providedDlcFiles)
        {
            provided.insert(file.device + ":/" + file.path);
        }
    }
    std::vector<std::string> warnings;
    std::unordered_set<std::string> reported;
    for (const Result& result : results)
    {
        for (const DlcFile& file : result.unresolvedDlcFiles)
        {
            const std::string key = file.device + ":/" + file.path;
            if (!provided.contains(key) && reported.insert(key).second)
            {
                warnings.push_back(file.warning);
            }
        }
    }
    return warnings;
}
} // namespace spl::mods
