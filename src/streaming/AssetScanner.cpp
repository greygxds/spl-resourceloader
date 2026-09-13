#include "streaming/AssetScanner.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "resource/Resource.h"
#include "streaming/AssetType.h"
#include "streaming/RscHeader.h"
#include "streaming/StreamAsset.h"
#include "util/Strings.h"

namespace spl::streaming
{
namespace
{
/// Extensions that belong to something other than streaming, so their presence is normal and
/// says nothing about the resource: FiveM's server-side companion files and editor leftovers.
constexpr std::array<std::string_view, 3> kIgnoredExtensions{"stream_raw", "txt", "md"};
constexpr std::array<std::string_view, 2> kIgnoredFileNames{"desktop.ini", "thumbs.db"};

/// Files that end up in stream/ by mistake and that no game store takes: listed as unsupported
/// rather than sent to the game one by one to be refused.
constexpr std::array<std::string_view, 21> kNonStreamingExtensions{
    "png", "jpg",  "jpeg", "dds", "bmp", "psd", "xml", "json", "lua", "js",  "ini",
    "cfg", "meta", "dat",  "zip", "7z",  "rar", "exe", "dll",  "bak", "html"};

/// The game's own loading spinner, which FiveM never registers (LoadStreamingFile.cpp:1848).
constexpr std::string_view kBusySpinnerFileName = "busy_spinner.gfx";

constexpr uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

struct DirectoryEntries
{
    std::vector<std::filesystem::path> directories;

    /// Entries rather than paths: on Windows the listing already carries each file's size and
    /// write time, so keeping them saves two filesystem calls per asset.
    std::vector<std::filesystem::directory_entry> files;
};

/// One directory level, each list sorted case-insensitively by name. Sorting is what makes
/// "the first path wins" a rule rather than a coin toss.
DirectoryEntries ListSorted(const std::filesystem::path& directory,
                            std::vector<std::string>& warnings, std::string_view resourceName)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    if (error)
    {
        warnings.push_back(fmt::format("{}: cannot read '{}': {}", resourceName,
                                       util::ToUtf8Generic(directory), error.message()));
        return {};
    }

    DirectoryEntries entries;
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        std::error_code entryError;
        if (entry.is_directory(entryError))
        {
            entries.directories.push_back(entry.path());
        }
        else if (entry.is_regular_file(entryError))
        {
            entries.files.push_back(entry);
        }
    }

    const auto byLoweredName =
        [](const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return util::ToLower(util::ToUtf8(left.filename())) <
               util::ToLower(util::ToUtf8(right.filename()));
    };
    std::ranges::sort(entries.directories, byLoweredName);
    std::ranges::sort(entries.files, byLoweredName, &std::filesystem::directory_entry::path);
    return entries;
}

/// The extension without its dot, lower-cased. Empty when the name carries none.
std::string LoweredExtension(const std::filesystem::path& file)
{
    std::string extension = util::ToUtf8(file.extension());
    if (extension.starts_with('.'))
    {
        extension.erase(0, 1);
    }
    return util::ToLower(extension);
}

/// "prop_a.ydr" without its extension. The name is flat or folder/file, and only the file part
/// ever carries a dot.
std::string_view WithoutExtension(std::string_view fileName)
{
    const std::size_t dot = fileName.find_last_of('.');
    return dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
}

/// Fills in what the filesystem knows about the file. Size and time feed the size
/// checks and let a changed file be noticed.
void ReadFileFacts(StreamAsset& asset, const std::filesystem::directory_entry& entry)
{
    std::error_code error;
    const std::uintmax_t size = entry.file_size(error);
    if (!error)
    {
        asset.fileSizeBytes = static_cast<uint64_t>(size);
    }

    std::error_code timeError;
    const std::filesystem::file_time_type written = entry.last_write_time(timeError);
    if (!timeError)
    {
        asset.lastWriteTime = written;
    }
}

void Skip(StreamAsset& asset, AssetDisposition disposition, std::string reason)
{
    asset.disposition = disposition;
    asset.dispositionReason = std::move(reason);
}

/// Reads and validates the RSC header of an asset whose type is supposed to have one. Only
/// the missing-header case can take the asset out of the plan; everything else is advice.
void ValidateRscHeader(StreamAsset& asset, const AssetTypeInfo& info,
                       const AssetScanner::Options& options, std::vector<std::string>& warnings)
{
    std::string error;
    asset.rsc = ReadRscHeader(asset.absolutePath, &error);
    if (!asset.rsc)
    {
        Skip(asset, AssetDisposition::SkippedInvalid, "the file could not be read");
        warnings.push_back(fmt::format("{}: {} — skipped", asset.resourceName, error));
        return;
    }

    if (!asset.rsc->IsResource())
    {
        if (info.acceptsPsoMetadata && asset.rsc->IsPsoMetadata())
        {
            return; // a PSO file: no page flags or version to check
        }
        if (!options.validateRscHeaders)
        {
            return; // the user turned validation off: take the file at face value
        }

        const std::string_view expected = info.acceptsPsoMetadata ? "RSC7 or PSIN" : "RSC7";
        Skip(asset, AssetDisposition::SkippedInvalid,
             fmt::format("not a compiled RAGE resource (missing {} header)", expected));
        warnings.push_back(
            fmt::format("{}: '{}' is not a compiled RAGE resource (missing {} header) — skipped",
                        asset.resourceName, asset.relativePath, expected));
        return;
    }

    if (!options.validateRscHeaders)
    {
        return;
    }

    // A version we do not recognize usually means a file exported for a different game build.
    // It may still load, so this is advice, never a rejection.
    if (info.knownRscVersion != 0 && asset.rsc->version != info.knownRscVersion)
    {
        warnings.push_back(fmt::format("{}: '{}' has resource version {}, expected {} for a .{}",
                                       asset.resourceName, asset.relativePath, asset.rsc->version,
                                       info.knownRscVersion, info.extension));
    }

    const uint64_t thresholdBytes =
        static_cast<uint64_t>(options.assetSizeWarningMiB) * kBytesPerMiB;
    if (thresholdBytes == 0)
    {
        return;
    }

    const uint64_t virtualBytes = asset.rsc->VirtualSizeBytes();
    const uint64_t physicalBytes = asset.rsc->PhysicalSizeBytes();
    if (virtualBytes > thresholdBytes || physicalBytes > thresholdBytes)
    {
        warnings.push_back(
            fmt::format("{}: '{}' needs {} MiB virtual and {} MiB physical memory, above the {} "
                        "MiB warning threshold",
                        asset.resourceName, asset.relativePath, virtualBytes / kBytesPerMiB,
                        physicalBytes / kBytesPerMiB, options.assetSizeWarningMiB));
    }
}
} // namespace

bool IsIgnoredStreamFile(const std::filesystem::path& file)
{
    const std::string name = util::ToLower(util::ToUtf8(file.filename()));
    if (std::ranges::find(kIgnoredFileNames, name) != kIgnoredFileNames.end())
    {
        return true;
    }

    // FiveM asks "why is this in stream/?" about extension-less files and ignores them; so do
    // we, because there is no streaming module to look up without an extension.
    const std::string extension = LoweredExtension(file);
    if (extension.empty())
    {
        return true;
    }

    return std::ranges::find(kIgnoredExtensions, extension) != kIgnoredExtensions.end();
}

std::string ToStreamingFileName(std::string_view fileName)
{
    std::string name{fileName};
    if (const std::size_t caret = name.find('^'); caret != std::string::npos)
    {
        name[caret] = '/';
    }
    return name;
}

AssetScanner::Result AssetScanner::Scan(const resource::Resource& resource,
                                        resource::ResourceId owner, const Options& options)
{
    Result result;

    const std::filesystem::path streamRoot = resource.GetStreamPath();
    std::error_code rootError;
    if (!std::filesystem::is_directory(streamRoot, rootError))
    {
        return result; // no stream/ folder: a resource may carry nothing but data files
    }

    const std::filesystem::path& resourceRoot = resource.GetRootPath();
    const std::string& resourceName = resource.GetName();

    std::queue<std::filesystem::path> pending;
    pending.push(streamRoot);

    // Canonical paths already walked, so a junction pointing at an ancestor cannot send the
    // walk round in circles (ResourceScanner guards the same way).
    std::set<std::filesystem::path> visited;

    while (!pending.empty())
    {
        const std::filesystem::path current = std::move(pending.front());
        pending.pop();

        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(current, error);
        if (!visited.insert(error ? current : canonical).second)
        {
            continue;
        }

        const DirectoryEntries entries = ListSorted(current, result.warnings, resourceName);

        for (const std::filesystem::path& directory : entries.directories)
        {
            // Hidden folders, which covers .git and editor caches. Bracketed folders are *not*
            // skipped here: inside stream/ they are ordinary subfolders, and packs do use
            // stream/[props]/.
            if (util::ToUtf8(directory.filename()).starts_with('.'))
            {
                continue;
            }
            pending.push(directory);
        }

        for (const std::filesystem::directory_entry& entry : entries.files)
        {
            const std::filesystem::path& file = entry.path();
            if (IsIgnoredStreamFile(file))
            {
                result.notes.push_back(
                    fmt::format("{}: ignoring '{}' (not a streaming file)", resourceName,
                                util::ToUtf8Generic(file.lexically_relative(resourceRoot))));
                continue;
            }

            const std::string fileName =
                ToStreamingFileName(util::ToLower(util::ToUtf8(file.filename())));
            StreamAsset asset{.owner = owner,
                              .resourceName = resourceName,
                              .absolutePath = file,
                              .relativePath =
                                  util::ToUtf8Generic(file.lexically_relative(resourceRoot)),
                              .fileName = fileName,
                              .streamingName = std::string{WithoutExtension(fileName)},
                              .extension = LoweredExtension(file),
                              .type = Classify(LoweredExtension(file))};
            ReadFileFacts(asset, entry);
            if (asset.type == AssetType::Unknown && asset.extension != "rpf" &&
                std::ranges::find(kNonStreamingExtensions, asset.extension) ==
                    kNonStreamingExtensions.end())
            {
                asset.type = AssetType::OtherModule;
            }

            const AssetTypeInfo& info = GetAssetTypeInfo(asset.type);
            if (asset.type != AssetType::Unknown && asset.fileSizeBytes == 0)
            {
                // An empty file is a failed copy or export; the game would read past its end.
                Skip(asset, AssetDisposition::SkippedInvalid, "the file is empty");
                result.warnings.push_back(
                    fmt::format("{}: '{}' is empty — skipped", resourceName, asset.relativePath));
            }
            else if (asset.type != AssetType::Unknown &&
                     asset.fileSizeBytes > AssetScanner::kMaxFileSizeBytes)
            {
                Skip(asset, AssetDisposition::SkippedInvalid,
                     fmt::format("the file is larger than {} MiB",
                                 AssetScanner::kMaxFileSizeBytes / kBytesPerMiB));
                result.warnings.push_back(fmt::format(
                    "{}: '{}' is {} MiB, above the {} MiB limit — skipped", resourceName,
                    asset.relativePath, asset.fileSizeBytes / kBytesPerMiB,
                    AssetScanner::kMaxFileSizeBytes / kBytesPerMiB));
            }
            else if (LoweredExtension(file) == "rpf")
            {
                Skip(asset, AssetDisposition::SkippedUnsupported,
                     "RPF archives are not supported in stream/");
                result.warnings.push_back(
                    fmt::format("{}: '{}' is an RPF archive; extract it, RPFs in stream/ are not "
                                "supported",
                                resourceName, asset.relativePath));
            }
            else if (asset.extension == "ysc")
            {
                Skip(asset, AssetDisposition::SkippedUnsupported,
                     "scripts are not run by this loader");
            }
            else if (asset.fileName == kBusySpinnerFileName)
            {
                Skip(asset, AssetDisposition::SkippedUnsupported,
                     "the game's loading spinner is never replaced");
            }
            else if (asset.type == AssetType::Unknown)
            {
                Skip(asset, AssetDisposition::SkippedUnsupported,
                     fmt::format("'.{}' is not a streaming file type", LoweredExtension(file)));
            }
            else if (info.tier == SupportTier::Planned)
            {
                Skip(asset, AssetDisposition::SkippedUnsupported,
                     fmt::format("support for .{} files is planned, but not implemented yet",
                                 info.extension));
            }
            else if (info.expectsRscHeader)
            {
                ValidateRscHeader(asset, info, options, result.warnings);
            }

            result.assets.push_back(std::move(asset));
        }
    }

    return result;
}
} // namespace spl::streaming
