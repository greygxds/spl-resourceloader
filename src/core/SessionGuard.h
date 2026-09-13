#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/LoaderConfig.h"
#include "core/Result.h"

namespace spl
{
/// What running.marker says the previous session was doing when it ended without cleaning up.
/// resource and file are empty when the session died before anything named them.
struct CrashMarker
{
    std::string stage;    ///< "early registration"
    std::string resource; ///< the resource being registered, when known
    std::string file;     ///< the file being registered, when known

    bool operator==(const CrashMarker&) const = default;
};

/// How this session starts, decided from what the previous one left behind.
struct SessionStartup
{
    /// The previous session crashed while it was changing game state.
    std::optional<CrashMarker> previousCrash;

    /// Register nothing: the previous session crashed and nobody knows which resource did it.
    bool safeMode = false;

    /// The resource this startup added to the quarantine, when the crash named one.
    std::string newlyQuarantined;

    /// Every resource to skip this session, newlyQuarantined included.
    std::vector<std::string> quarantined;

    /// state.toml could not be read; the message is ready to log.
    std::string stateError;
};

/// Keeps track of whether the loader is in the middle of changing game state, so that a crash
/// there is noticed on the next launch. Files, all in the data folder:
///
/// - running.marker exists only while registration is under way. A marker found at startup
///   means the previous session never got to remove it.
/// - state.toml lists quarantined resources. The loader writes it and never touches
///   config.toml; deleting a name from it lets the resource load again.
///
/// Pure filesystem work, so it is unit-tested; the crash handler only calls RecordCrash().
class SessionGuard
{
public:
    SessionGuard() = default;
    SessionGuard(std::filesystem::path markerFile, std::filesystem::path stateFile);

    /// Reads what the previous session left, decides how this one starts, and removes the old
    /// marker, so a crash only ever costs one launch. A newly quarantined resource is written
    /// to state.toml straight away.
    [[nodiscard]] SessionStartup Begin(config::SafeMode mode);

    /// Registration is about to touch game state. Writes the marker once; later calls are free
    /// until MarkIdle().
    void MarkBusy(std::string_view stage);

    /// Nothing is being changed right now (waiting, finished, or shutting down cleanly).
    void MarkIdle();

    /// Overwrites the marker with what was running at the moment of a crash. Called from the
    /// crash handler, so it only formats a short string and writes one file.
    void RecordCrash(const CrashMarker& marker) const;

    /// Adds a resource to state.toml. Idempotent and case-insensitive.
    void Quarantine(std::string_view resourceName);

    [[nodiscard]] bool IsBusy() const
    {
        return m_busy;
    }

    [[nodiscard]] const std::vector<std::string>& GetQuarantined() const
    {
        return m_quarantined;
    }

    /// The decision Begin() makes, without the files.
    [[nodiscard]] static SessionStartup Decide(std::optional<CrashMarker> marker,
                                               config::SafeMode mode,
                                               std::vector<std::string> quarantined);

    /// "stage=...\nresource=...\nfile=...\n". Unknown lines are ignored when parsing, and a
    /// marker with no stage still counts: an empty file is what a crash mid-write leaves.
    [[nodiscard]] static std::string FormatMarker(const CrashMarker& marker);
    [[nodiscard]] static CrashMarker ParseMarker(std::string_view text);

    /// state.toml. Parsing fails with a message instead of throwing; names are trimmed and
    /// deduplicated case-insensitively.
    [[nodiscard]] static std::string FormatState(const std::vector<std::string>& quarantined);
    [[nodiscard]] static Result<std::vector<std::string>> ParseState(std::string_view tomlText);

private:
    std::filesystem::path m_markerFile;
    std::filesystem::path m_stateFile;
    std::vector<std::string> m_quarantined;
    bool m_busy = false;
};
} // namespace spl
