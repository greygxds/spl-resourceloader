#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "resource/Resource.h"
#include "streaming/StreamAsset.h"

namespace spl::streaming
{
/// Walks one resource's stream/ folder and reports the assets it holds.
///
/// Pure filesystem work, like ResourceScanner: no configuration is applied beyond the
/// validation options below, nothing is logged, and conflicts between resources are left to
/// StreamingPlan.
class AssetScanner
{
public:
    /// No streamed file is anywhere near this; a bigger one is corrupt or not an asset, and the
    /// raw streamer would have to read all of it.
    static constexpr uint64_t kMaxFileSizeBytes = 256ULL * 1024ULL * 1024ULL;

    struct Options
    {
        /// diagnostics.validate_rsc_headers. With it off, a file without an RSC header is
        /// still planned, on the assumption that the user knows what they are doing.
        bool validateRscHeaders = true;

        /// diagnostics.asset_size_warning_mib. An asset whose virtual or physical size is
        /// above this earns a warning; the game has to find that memory at runtime.
        uint32_t assetSizeWarningMiB = 256;
    };

    struct Result
    {
        /// Every file worth reporting, in scan order: breadth-first, each level sorted
        /// case-insensitively, so which of two duplicates wins never depends on the
        /// filesystem's enumeration order. Files with no streaming meaning at all
        /// (desktop.ini, *.txt, *.stream_raw) are left out entirely.
        std::vector<StreamAsset> assets;

        /// Problems worth the user's attention, already phrased for the log.
        std::vector<std::string> warnings;

        /// Detail the user only wants when they go looking: the files that were skipped
        /// without comment. Logged at debug.
        std::vector<std::string> notes;
    };

    [[nodiscard]] static Result Scan(const resource::Resource& resource, resource::ResourceId owner,
                                     const Options& options);
};

/// True for a file that has no business being in stream/ and is skipped without comment:
/// server companion files, editor leftovers and notes.
[[nodiscard]] bool IsIgnoredStreamFile(const std::filesystem::path& file);

/// The name the game knows a stream file by. A clothing pack cannot ship the folder a DLC
/// collection's drawables live in, so FiveM spells it with '^' and turns the first one back
/// into '/': "mp_f_freemode_01_mp_f_xmas^accs_000_u.ydd" is streamed as
/// "mp_f_freemode_01_mp_f_xmas/accs_000_u.ydd", the name the ped variation code asks for.
/// Any other name comes back unchanged.
[[nodiscard]] std::string ToStreamingFileName(std::string_view fileName);
} // namespace spl::streaming
