#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/Result.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"

namespace spl::rage
{
/// pgRawStreamer, the fiCollection every loose file lives in. Overrides need it directly,
/// because a slot that already has a handle cannot go through RegisterRawStreamingFile: the
/// entry is looked up by path and only the slot's handle changes (FiveM
/// LoadStreamingFile.cpp:1924).
class RawStreamerInterface
{
public:
    void Initialize(const GameAddresses& addresses);

    /// Gets the instance, finds its entry list and proves it on this build: the game's own
    /// first entry has to carry a VFS path, and GetEntryByName for that path has to give the
    /// same entry back without creating one. An error disables overrides only.
    [[nodiscard]] Result<void> Verify(const memory::Module& image);

    /// The entry for vfsPath, created when the raw streamer does not have one yet.
    /// std::nullopt when the game refused or the call faulted.
    [[nodiscard]] std::optional<uint16_t> GetEntryByName(std::string_view vfsPath) const;

    /// The path an entry was registered under, read from the entry list the way FiveM's
    /// replacement GetEntryNameToBuffer does. std::nullopt when it cannot be read.
    [[nodiscard]] std::optional<std::string> GetEntryPath(uint16_t entryIndex) const;

    /// How many entries the raw streamer holds, or std::nullopt when unreadable.
    [[nodiscard]] std::optional<uint32_t> GetEntryCount() const;

private:
    /// Where m_entries sits, decoded from GetEntryNameToBuffer. std::nullopt when that code is
    /// unavailable or has no clear chunk load, and FiveM's struct offset has to do.
    [[nodiscard]] std::optional<uint32_t> DecodeEntriesOffset(const memory::Module& image) const;

    /// The first of the game's own entries that has a path, for the verification.
    [[nodiscard]] std::optional<uint16_t> FindProbeEntry() const;

    /// Everything the verification looked at, at debug level, for the build notes.
    void LogLayout(const memory::Module& image) const;

    /// The game's own GetEntryName answer, which on retail builds is not the path. Diagnostics.
    [[nodiscard]] std::optional<std::string> GetEntryNameFromGame(uint16_t entryIndex) const;

    uintptr_t m_getInstance = 0;
    uintptr_t m_getEntryNameToBuffer = 0;
    void* m_instance = nullptr; ///< non-owning: the game's, for the whole process
    uint32_t m_entriesOffset = 0;
};
} // namespace spl::rage
