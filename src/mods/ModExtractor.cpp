#include "mods/ModExtractor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "core/Result.h"
#include "core/Version.h"
#include "mods/AssemblyParser.h"
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
void AddOverlayRoot(ModExtractor::Result& result, const OverlayFolder& folder,
                    std::string_view target)
{
    const std::filesystem::path path =
        result.root / folder.prefix.substr(0, folder.prefix.size() - 1);
    if (std::ranges::any_of(result.overlays, [&path](const ModExtractor::OverlayRoot& root)
                            { return root.folder == path; }))
    {
        return;
    }
    for (const std::string_view mountPoint : folder.mountPoints)
    {
        result.overlays.push_back(ModExtractor::OverlayRoot{
            .folder = path,
            .mountPoint = std::string{mountPoint},
            .probeFile = std::string{target.substr(folder.prefix.size())}});
    }
}

/// Bumped whenever what an extraction writes changes, so an old cache is extracted again.
constexpr int kCacheFormat = 3;

constexpr std::string_view kStampFileName = ".spl_extracted";

/// What an extraction depends on: the archive as it is on disk, the loader, and the game build
/// that gates content DLCs.
[[nodiscard]] std::string MakeStamp(const DiscoveredMod& mod, uint32_t gameBuild)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(mod.absolutePath, error);
    const auto written = std::filesystem::last_write_time(mod.absolutePath, error);
    return fmt::format("format {}\nloader {}\nsource {} {}\nbuild {}\n", kCacheFormat,
                       Version::Describe(), size, written.time_since_epoch().count(), gameBuild);
}

/// The result an earlier extraction recorded next to its files, when stamp still describes it.
[[nodiscard]] std::optional<ModExtractor::Result>
ReadCachedResult(const std::filesystem::path& root, const std::string& stamp)
{
    std::ifstream stream{root / kStampFileName, std::ios::binary};
    if (!stream)
    {
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    if (!text.starts_with(stamp))
    {
        return std::nullopt;
    }

    ModExtractor::Result result{.root = root, .reused = true};
    std::string_view rest = std::string_view{text}.substr(stamp.size());
    while (!rest.empty())
    {
        const std::size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const std::size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value =
            space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
        if (key == "files")
        {
            result.extractedFiles =
                static_cast<uint32_t>(std::strtoul(std::string{value}.c_str(), nullptr, 10));
        }
        else if (key == "overlay")
        {
            const std::size_t tab = value.find('\t');
            const std::string mountPoint{value.substr(0, tab)};
            const std::optional<OverlayFolder> folder = FindOverlayFolder(
                std::string{mountPoint.starts_with("common") ? "common/" : "platform/"} + "x");
            if (tab == std::string_view::npos || !folder)
            {
                return std::nullopt;
            }
            result.overlays.push_back(ModExtractor::OverlayRoot{
                .folder = root / folder->prefix.substr(0, folder->prefix.size() - 1),
                .mountPoint = mountPoint,
                .probeFile = std::string{value.substr(tab + 1)}});
        }
        else if (key == "warning")
        {
            result.warnings.emplace_back(value);
        }
        else if (key == "dlc-provided" || key == "dlc-unresolved")
        {
            // device \t path [\t warning]
            const std::size_t first = value.find('\t');
            if (first == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::size_t second = value.find('\t', first + 1);
            ModExtractor::DlcFile file{
                .device = std::string{value.substr(0, first)},
                .path = std::string{value.substr(first + 1, second == std::string_view::npos
                                                                ? std::string_view::npos
                                                                : second - first - 1)}};
            if (key == "dlc-provided")
            {
                result.providedDlcFiles.push_back(std::move(file));
                continue;
            }
            if (second == std::string_view::npos)
            {
                return std::nullopt;
            }
            file.warning = std::string{value.substr(second + 1)};
            result.unresolvedDlcFiles.push_back(std::move(file));
        }
    }
    return result;
}

/// One cache line's worth of text: the line and field separators cannot appear inside.
[[nodiscard]] std::string SingleField(std::string_view text)
{
    std::string single{text};
    std::ranges::replace(single, '\n', ' ');
    std::ranges::replace(single, '\t', ' ');
    return single;
}

/// Records result next to the files it describes. A failure only costs the next launch a re-run.
void WriteCachedResult(const ModExtractor::Result& result, const std::string& stamp)
{
    std::string text = stamp + fmt::format("files {}\n", result.extractedFiles);
    for (const ModExtractor::OverlayRoot& overlay : result.overlays)
    {
        text += fmt::format("overlay {}\t{}\n", overlay.mountPoint, overlay.probeFile);
    }
    for (const std::string& warning : result.warnings)
    {
        std::string single = warning;
        std::ranges::replace(single, '\n', ' ');
        text += "warning " + single + '\n';
    }
    for (const ModExtractor::DlcFile& file : result.providedDlcFiles)
    {
        text +=
            fmt::format("dlc-provided {}\t{}\n", SingleField(file.device), SingleField(file.path));
    }
    for (const ModExtractor::DlcFile& file : result.unresolvedDlcFiles)
    {
        text += fmt::format("dlc-unresolved {}\t{}\t{}\n", SingleField(file.device),
                            SingleField(file.path), SingleField(file.warning));
    }
    std::ofstream stream{result.root / kStampFileName, std::ios::binary | std::ios::trunc};
    stream << text;
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

/// Cache folder names come from archive stems, which are user input.
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

struct Extraction
{
    ModExtractor::Result& result;
    std::string archiveName;
    std::unordered_set<std::string> streamNames;
    std::size_t packCount = 0;

    /// Reads an archive-relative file (nested RPF bytes, setup2.xml, ...) as text.
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

    bool WriteFile(const std::filesystem::path& file, const std::vector<std::byte>& bytes,
                   std::string_view what)
    {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (error)
        {
            result.warnings.push_back("'" + archiveName + "': cannot create '" + file.string() +
                                      "' (" + error.message() + ")");
            return false;
        }
        std::ofstream stream{file, std::ios::binary | std::ios::trunc};
        if (!stream)
        {
            result.warnings.push_back("'" + archiveName + "': cannot write '" + file.string() +
                                      "' (" + std::string{what} + ")");
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream.good())
        {
            result.warnings.push_back("'" + archiveName + "': failed writing '" + file.string() +
                                      "'");
            return false;
        }
        ++result.extractedFiles;
        return true;
    }

    /// The top-level files of a nested archive, which FiveM registers under the archive's own tag
    /// (ModVFSDevice.cpp:286-352). A name an earlier pack of the mod already provided is left to
    /// that pack, except a .ymf: every pack has its own _manifest.ymf, so each gets a folder.
    void ExtractPack(const rpf::RpfReader& pack)
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
            spl::Result<std::vector<std::byte>> bytes = pack.ReadFile(node.path);
            if (!bytes)
            {
                result.warnings.push_back("'" + archiveName + "': cannot read '" + node.path +
                                          "' in '" + pack.GetPath().filename().string() + "' (" +
                                          bytes.GetMessage() + ")");
                continue;
            }
            const std::filesystem::path folder =
                manifest ? result.root / "stream" / fmt::format("pack{}", packCount)
                         : result.root / "stream";
            WriteFile(folder / node.path, bytes.GetValue(), node.path);
        }
    }

    /// stream/<basename>, first source wins so two targets never overwrite each other.
    /// source is archive-relative already (callers prepend content/ for assembly sources).
    void ExtractStreamed(const rpf::RpfReader& archive, std::string_view source,
                         std::string_view target)
    {
        const std::string name = BaseName(target);
        if (!streamNames.insert(name).second)
        {
            result.warnings.push_back("'" + archiveName + "': '" + std::string{source} +
                                      "' skipped, '" + name + "' is already provided");
            return;
        }
        spl::Result<std::vector<std::byte>> bytes = archive.ReadFile(source);
        if (!bytes)
        {
            result.warnings.push_back("'" + archiveName + "': cannot read '" + std::string{source} +
                                      "' (" + bytes.GetMessage() + ")");
            return;
        }
        WriteFile(result.root / "stream" / name, bytes.GetValue(), source);
    }
};

/// Opens a nested archive, warning on behalf of mod when it fails.
[[nodiscard]] std::optional<rpf::RpfReader> OpenNested(ModExtractor::Result& result,
                                                       std::string_view modName,
                                                       const rpf::RpfReader& archive,
                                                       std::string_view source)
{
    spl::Result<rpf::RpfReader> pack = archive.OpenNested(source);
    if (!pack)
    {
        result.warnings.push_back("'" + std::string{modName} + "': cannot open nested '" +
                                  std::string{source} + "' (" + pack.GetMessage() + ")");
        return std::nullopt;
    }
    return std::move(pack.GetValue());
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

void ExtractDlcs(const DiscoveredMod& mod, const rpf::RpfReader& archive, const ModOverlay& overlay,
                 uint32_t gameBuild, Extraction& extraction, std::string& manifest)
{
    struct PendingDlc
    {
        DlcDescriptor descriptor;
        std::string device; ///< lower-case deviceName
        std::string source; ///< lower-case, so dlc.rpf sorts before dlc1.rpf
        rpf::RpfReader pack;
    };
    std::vector<PendingDlc> pending;
    for (const ModOverlay::DlcJob& job : overlay.DlcJobs())
    {
        std::optional<rpf::RpfReader> pack =
            OpenNested(extraction.result, mod.name, archive, ResolveSource(job.source));
        if (!pack)
        {
            continue;
        }
        const std::optional<std::string> setup = Extraction::ReadText(*pack, "setup2.xml");
        if (!setup)
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                                 "' has no setup2.xml, skipped");
            continue;
        }
        const Setup2Result parsed = AssemblyParser::ParseSetup2(*setup);
        if (parsed.fatal)
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                                 "' has a broken setup2.xml, skipped");
            continue;
        }
        if (parsed.descriptor.deviceName.empty())
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                                 "' names no device, skipped");
            continue;
        }
        if (!VersionAllows(parsed.descriptor.requiredVersion, gameBuild))
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" + job.source +
                                                 "' needs game build " +
                                                 parsed.descriptor.requiredVersion + ", skipped");
            continue;
        }
        pending.push_back(PendingDlc{.descriptor = parsed.descriptor,
                                     .device = util::ToLower(parsed.descriptor.deviceName),
                                     .source = util::ToLower(job.source),
                                     .pack = std::move(*pack)});
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
        const std::optional<std::string> content = Extraction::ReadText(dlc.pack, "content.xml");
        if (!content)
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" +
                                                 dlc.descriptor.deviceName +
                                                 "' has no content.xml, skipped");
            continue;
        }
        const ContentResult parsed = AssemblyParser::ParseContent(*content);
        if (parsed.fatal)
        {
            extraction.result.warnings.push_back("'" + mod.name + "': '" +
                                                 dlc.descriptor.deviceName +
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
                extraction.result.warnings.push_back("'" + mod.name + "': '" + item.filename +
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
            if (candidate.device == device && candidate.pack.Contains(relative))
            {
                return &candidate.pack;
            }
        }
        return nullptr;
    };
    std::unordered_set<std::string> handled; ///< "<device>:/<lower-case path>"
    const auto firstTime = [&handled](const PendingItem& entry)
    { return handled.insert(entry.dlc->device + ":/" + util::ToLower(entry.relative)).second; };
    const auto provide = [&extraction](const PendingItem& entry)
    {
        extraction.result.providedDlcFiles.push_back(ModExtractor::DlcFile{
            .device = entry.dlc->device, .path = util::ToLower(entry.relative)});
    };
    const auto leaveUnresolved = [&extraction, &mod](const PendingItem& entry)
    {
        // Reported only when no other mod provides it either (ReportUnresolvedDlcFiles).
        extraction.result.unresolvedDlcFiles.push_back(ModExtractor::DlcFile{
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
        if (const std::optional<rpf::RpfReader> pack =
                OpenNested(extraction.result, mod.name, *owner, entry.relative))
        {
            extraction.ExtractPack(*pack);
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
            if (!IsStreamed(extraction.streamNames, name))
            {
                // A loose .ytyp in the pack itself streams like any other.
                std::string loose = entry.relative;
                loose.replace(loose.size() - name.size(), name.size(), name);
                if (const rpf::RpfReader* const owner = findPack(entry.dlc->device, loose))
                {
                    const spl::Result<std::vector<std::byte>> bytes = owner->ReadFile(loose);
                    if (bytes && extraction.WriteFile(extraction.result.root / "stream" / name,
                                                      bytes.GetValue(), entry.item.filename))
                    {
                        extraction.streamNames.insert(name);
                    }
                }
            }
            provide(entry);
            // Requested even when no pack has it: it may name a .ytyp of the game, of another
            // DLC or of another mod, which the streaming plan checks and reports.
            const std::string target =
                IsStreamed(extraction.streamNames, name) ? "stream/" + name : name;
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
        const spl::Result<std::vector<std::byte>> bytes = owner->ReadFile(entry.relative);
        if (!bytes)
        {
            extraction.result.warnings.push_back(
                "'" + mod.name + "': cannot read '" + entry.relative + "' in '" +
                entry.dlc->descriptor.deviceName + "' (" + bytes.GetMessage() + ")");
            continue;
        }
        provide(entry);
        const std::string target =
            "__dlc__/" + entry.dlc->descriptor.deviceName + "/" + entry.relative;
        if (!extraction.WriteFile(extraction.result.root / target, bytes.GetValue(),
                                  entry.item.filename))
        {
            continue;
        }
        manifest += "data_file '" + entry.item.fileType + "' '" + EscapeLua(target) + "'\n";
    }
}
} // namespace

spl::Result<void> ModExtractor::PruneCache(const std::filesystem::path& cacheRoot,
                                           std::span<const DiscoveredMod> mods)
{
    std::error_code error;
    std::filesystem::create_directories(cacheRoot, error);
    if (error)
    {
        return MakeError(ErrorCode::Io, "cannot prepare the mods cache '{}' ({})",
                         cacheRoot.string(), error.message());
    }
    std::unordered_set<std::string> kept;
    for (const DiscoveredMod& mod : mods)
    {
        kept.insert(util::ToLower(SanitizeName(mod.name)));
    }
    for (std::filesystem::directory_iterator entry{cacheRoot, error}, end; entry != end && !error;
         entry.increment(error))
    {
        if (!kept.contains(util::ToLower(entry->path().filename().string())))
        {
            std::error_code removeError;
            std::filesystem::remove_all(entry->path(), removeError);
        }
    }
    return {};
}

ModExtractor::Result ModExtractor::Extract(const DiscoveredMod& mod,
                                           const std::filesystem::path& cacheRoot,
                                           uint32_t gameBuild)
{
    Result result{.root = cacheRoot / SanitizeName(mod.name)};
    const std::string stamp = MakeStamp(mod, gameBuild);
    if (std::optional<Result> cached = ReadCachedResult(result.root, stamp))
    {
        return std::move(*cached);
    }

    std::error_code error;
    std::filesystem::remove_all(result.root, error); // a stale or half-written extraction
    error.clear();
    std::filesystem::create_directories(result.root, error);
    if (error)
    {
        result.warnings.push_back("cannot create '" + result.root.string() + "' (" +
                                  error.message() + ")");
        return result;
    }

    spl::Result<rpf::RpfReader> opened = rpf::RpfReader::Open(mod.absolutePath);
    if (!opened)
    {
        result.warnings.push_back("cannot open '" + mod.absolutePath.string() + "' (" +
                                  opened.GetMessage() + ")");
        return result;
    }
    const rpf::RpfReader& archive = opened.GetValue();
    const ModOverlay overlay = ModOverlay::Build(mod.package);
    Extraction extraction{.result = result, .archiveName = mod.name};

    for (const ModOverlay::StreamCandidate& candidate : overlay.StreamCandidates())
    {
        extraction.ExtractStreamed(archive, ResolveSource(candidate.source), candidate.target);
    }
    for (const ModOverlay::Mapping& map : overlay.MapTargets())
    {
        extraction.ExtractStreamed(archive, ResolveSource(map.source), map.target);
    }
    for (const ModOverlay::Mapping& manifest : overlay.ManifestTargets())
    {
        extraction.ExtractStreamed(archive, ResolveSource(manifest.source), manifest.target);
    }

    for (const ModOverlay::FauxPackJob& job : overlay.FauxPackJobs())
    {
        if (const std::optional<rpf::RpfReader> pack =
                OpenNested(result, mod.name, archive, ResolveSource(job.source)))
        {
            extraction.ExtractPack(*pack);
        }
    }

    // Every game file the mod replaces, served through the overlay mounts. FiveM never mounts a
    // target outside common/ and platform/ either.
    std::unordered_set<std::string> overlayTargets;
    for (const ModOverlay::Mapping& mapping : overlay.Mappings())
    {
        const std::optional<OverlayFolder> folder = FindOverlayFolder(mapping.target);
        if (!folder || overlayTargets.contains(util::ToLower(mapping.target)))
        {
            continue;
        }
        const spl::Result<std::vector<std::byte>> bytes =
            archive.ReadFile(ResolveSource(mapping.source));
        if (!bytes)
        {
            result.warnings.push_back("'" + mod.name + "': cannot read '" + mapping.source + "' (" +
                                      bytes.GetMessage() + ")");
            continue;
        }
        if (!extraction.WriteFile(result.root / mapping.target, bytes.GetValue(), mapping.source))
        {
            continue;
        }
        overlayTargets.insert(util::ToLower(mapping.target));
        AddOverlayRoot(result, *folder, mapping.target);
    }

    // The game has read its startup metas before the loader runs, so the ones whose type is
    // known are loaded as data files as well.
    std::string manifest = "-- Generated from '" + EscapeLua(mod.absolutePath.filename().string()) +
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
    ExtractDlcs(mod, archive, overlay, gameBuild, extraction, manifest);

    std::ofstream stream{result.root / "fxmanifest.lua", std::ios::binary | std::ios::trunc};
    stream << manifest;
    if (!stream.good())
    {
        result.warnings.push_back("'" + mod.name + "': cannot write its manifest");
        return result;
    }
    stream.close();
    WriteCachedResult(result, stamp); // last, so an interrupted extraction is never reused
    return result;
}

std::vector<std::string> ModExtractor::ReportUnresolvedDlcFiles(std::span<const Result> results)
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
