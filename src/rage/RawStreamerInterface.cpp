#include "rage/RawStreamerInterface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "rage/ReadableMemory.h"
#include "rage/SafeCall.h"
#include "rage/types/FileDeviceTypes.h"
#include "rage/types/VirtualCall.h"

namespace spl::rage
{
namespace
{
using GetInstanceFn = void* (*)();
using GetEntryNameFn = const char* (*)(void* self, uint16_t index);
using GetEntryByNameFn = uint16_t (*)(void* self, const char* name);

/// Every build registers loose files of its own before a script runs (build 3889 had 0x44 of
/// them). Entry 0 has no path on 3889, so the probe takes the first of these that does.
constexpr uint16_t kProbeEntryLimit = 16;

/// A VFS path names its device: "platform:/...", "update:/...".
constexpr std::string_view kDeviceSeparator = ":/";

/// Entries and vtable slots LogLayout shows.
constexpr uint16_t kLoggedEntries = 3;
constexpr std::size_t kFirstLoggedSlot = FileDeviceLayout::kKnownSlotCount;
constexpr std::size_t kLoggedSlots = 10;

/// pgRawStreamer holds uint16_t entry indices.
constexpr uint32_t kMaxEntryCount = 0xFFFF;

template <typename T> [[nodiscard]] std::optional<T> ReadValue(uintptr_t address)
{
    if (!IsReadableMemory(address, sizeof(T)))
    {
        return std::nullopt;
    }
    return SafeCall("rage::pgRawStreamer::Read",
                    [&]
                    {
                        T value{};
                        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
                        return value;
                    });
}

/// A terminated string of 1 to kMaxEntryNameLength characters at address.
[[nodiscard]] std::optional<std::string> ReadName(uintptr_t address)
{
    const std::size_t available =
        std::min(ReadableBytesAt(address), CollectionLayout::kMaxEntryNameLength);
    if (available == 0)
    {
        return std::nullopt;
    }
    const std::optional<std::optional<std::string>> name =
        SafeCall("rage::pgRawStreamer::ReadName",
                 [&]() -> std::optional<std::string>
                 {
                     const auto* const text = reinterpret_cast<const char*>(address);
                     const std::size_t length = strnlen_s(text, available);
                     if (length == 0 || length == available)
                     {
                         return std::nullopt;
                     }
                     return std::string(text, length);
                 });
    return name ? *name : std::nullopt;
}

[[nodiscard]] std::string FormatAddress(const memory::Module& image, uintptr_t address)
{
    if (!image.Contains(address))
    {
        return fmt::format("{:#x}", address);
    }
    return fmt::format("{}+{:#x}", image.GetFileName(), address - image.GetBase());
}

[[nodiscard]] std::string ToHex(std::span<const uint8_t> bytes)
{
    std::string text;
    for (const uint8_t byte : bytes)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += fmt::format("{:02X}", byte);
    }
    return text;
}

/// Printable, or the first bytes in hex: what a name getter returned, for the log.
[[nodiscard]] std::string Describe(const std::optional<std::string>& name)
{
    if (!name)
    {
        return "unreadable";
    }
    const bool printable = std::ranges::all_of(*name, [](char character)
                                               { return character >= 0x20 && character < 0x7F; });
    return printable ? fmt::format("'{}'", *name)
                     : fmt::format("bytes {}",
                                   ToHex(std::span{reinterpret_cast<const uint8_t*>(name->data()),
                                                   std::min<std::size_t>(name->size(), 16)}));
}
} // namespace

void RawStreamerInterface::Initialize(const GameAddresses& addresses)
{
    m_getInstance = addresses.pgRawStreamerGetInstance;
    m_getEntryNameToBuffer = addresses.pgRawStreamerGetEntryNameToBuffer;
    m_instance = nullptr;
    m_entriesOffset = 0;
}

Result<void> RawStreamerInterface::Verify(const memory::Module& image)
{
    if (!image.Contains(m_getInstance))
    {
        return MakeError(ErrorCode::NotFound,
                         "signature 'rage::pgRawStreamer::GetInstance' did not resolve");
    }

    const auto getInstance = reinterpret_cast<GetInstanceFn>(m_getInstance);
    const std::optional<void*> instance =
        SafeCall("rage::pgRawStreamer::GetInstance", [&] { return getInstance(); });
    if (!instance || *instance == nullptr)
    {
        return MakeError(ErrorCode::Unavailable, "the raw streamer does not exist yet");
    }
    const std::optional<uintptr_t> vtable =
        ReadValue<uintptr_t>(reinterpret_cast<uintptr_t>(*instance));
    if (!vtable || !image.Contains(*vtable))
    {
        return MakeError(ErrorCode::NotFound, "the raw streamer has no vtable in {}",
                         image.GetFileName());
    }
    m_instance = *instance;

    const std::optional<uint32_t> decoded = DecodeEntriesOffset(image);
    m_entriesOffset = decoded.value_or(CollectionLayout::kFallbackEntriesOffset);
    SPL_LOG_DEBUG(Rage, "Raw streamer at {:#x}, entry list at +{:#x} ({})",
                  reinterpret_cast<uintptr_t>(m_instance), m_entriesOffset,
                  decoded ? "read from GetEntryNameToBuffer" : "FiveM's struct offset");
    LogLayout(image);

    const auto refuse = [this](Error error)
    {
        m_instance = nullptr;
        return error;
    };

    const std::optional<uint32_t> count = GetEntryCount();
    if (!count || *count == 0 || *count > kMaxEntryCount)
    {
        return refuse(MakeError(ErrorCode::NotFound,
                                "the raw streamer's entry list at +{:#x} has no plausible count, "
                                "so the fiCollection layout is wrong for this build",
                                m_entriesOffset));
    }
    const std::optional<uint16_t> probe = FindProbeEntry();
    if (!probe)
    {
        return refuse(MakeError(ErrorCode::NotFound,
                                "none of the first {} raw streamer entries at +{:#x} has a "
                                "path-like name, so the fiCollection layout is wrong for this "
                                "build",
                                kProbeEntryLimit, m_entriesOffset));
    }
    const std::string path = GetEntryPath(*probe).value_or(std::string{});

    const std::optional<uintptr_t> getEntryByName =
        ReadValue<uintptr_t>(*vtable + CollectionLayout::kSlotGetEntryByName * sizeof(uintptr_t));
    if (!getEntryByName || !image.Contains(*getEntryByName))
    {
        return refuse(MakeError(ErrorCode::NotFound, "fiCollection slot {} does not point into {}",
                                CollectionLayout::kSlotGetEntryByName, image.GetFileName()));
    }
    const std::optional<uint16_t> index = GetEntryByName(path);
    const std::optional<uint32_t> countAfter = GetEntryCount();
    if (countAfter != count)
    {
        return refuse(MakeError(ErrorCode::NotFound,
                                "looking up '{}' changed the entry count from {} to {}, so "
                                "fiCollection slot {} is wrong for this build",
                                path, *count, countAfter.value_or(0),
                                CollectionLayout::kSlotGetEntryByName));
    }
    if (!index || *index != *probe)
    {
        return refuse(MakeError(ErrorCode::NotFound,
                                "looking up '{}' gave entry {} instead of {}, so fiCollection "
                                "slot {} is wrong for this build",
                                path, index ? static_cast<int>(*index) : -1, *probe,
                                CollectionLayout::kSlotGetEntryByName));
    }

    SPL_LOG_DEBUG(Rage, "Raw streamer verified: {} entries at +{:#x}, entry {} is '{}'", *count,
                  m_entriesOffset, *probe, path);
    return {};
}

std::optional<uint16_t> RawStreamerInterface::FindProbeEntry() const
{
    const uint16_t limit =
        static_cast<uint16_t>(std::min<uint32_t>(GetEntryCount().value_or(0), kProbeEntryLimit));
    for (uint16_t entry = 0; entry < limit; ++entry)
    {
        const std::optional<std::string> path = GetEntryPath(entry);
        if (path && path->find(kDeviceSeparator) != std::string::npos)
        {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> RawStreamerInterface::DecodeEntriesOffset(const memory::Module& image) const
{
    if (!image.Contains(m_getEntryNameToBuffer) ||
        !IsReadableMemory(m_getEntryNameToBuffer, CollectionLayout::kEntriesLoadSearchBytes))
    {
        return std::nullopt;
    }
    std::array<uint8_t, CollectionLayout::kEntriesLoadSearchBytes> code{};
    const bool read =
        SafeCall("rage::pgRawStreamer::GetEntryNameToBuffer",
                 [&]
                 {
                     std::memcpy(code.data(), reinterpret_cast<const void*>(m_getEntryNameToBuffer),
                                 code.size());
                 });
    if (!read)
    {
        return std::nullopt;
    }
    return CollectionLayout::FindEntriesOffset(code);
}

void RawStreamerInterface::LogLayout(const memory::Module& image) const
{
    const auto instance = reinterpret_cast<uintptr_t>(m_instance);
    if (image.Contains(m_getEntryNameToBuffer) && IsReadableMemory(m_getEntryNameToBuffer, 48))
    {
        std::array<uint8_t, 48> code{};
        std::memcpy(code.data(), reinterpret_cast<const void*>(m_getEntryNameToBuffer),
                    code.size());
        SPL_LOG_DEBUG(Rage, "GetEntryNameToBuffer at {}: {}",
                      FormatAddress(image, m_getEntryNameToBuffer), ToHex(code));
    }

    const std::optional<uint32_t> count = GetEntryCount();
    SPL_LOG_DEBUG(Rage, "Raw streamer entry count: {}",
                  count ? fmt::format("{} ({:#x})", *count, *count) : std::string{"unreadable"});
    const uint16_t logged =
        static_cast<uint16_t>(std::min<uint32_t>(count.value_or(0), kLoggedEntries));
    for (uint16_t entry = 0; entry < logged; ++entry)
    {
        SPL_LOG_DEBUG(Rage, "Raw streamer entry {}: path {}, GetEntryName {}", entry,
                      Describe(GetEntryPath(entry)), Describe(GetEntryNameFromGame(entry)));
    }

    const std::optional<uintptr_t> vtable = ReadValue<uintptr_t>(instance);
    if (!vtable)
    {
        return;
    }
    std::string slots;
    for (std::size_t slot = kFirstLoggedSlot; slot < kFirstLoggedSlot + kLoggedSlots; ++slot)
    {
        const std::optional<uintptr_t> function =
            ReadValue<uintptr_t>(*vtable + slot * sizeof(uintptr_t));
        slots += fmt::format("{}{}={}", slots.empty() ? "" : ", ", slot,
                             function ? FormatAddress(image, *function) : "unreadable");
    }
    SPL_LOG_DEBUG(Rage, "Raw streamer vtable {} slots: {}", FormatAddress(image, *vtable), slots);
}

std::optional<uint32_t> RawStreamerInterface::GetEntryCount() const
{
    if (m_instance == nullptr || HasFaulted())
    {
        return std::nullopt;
    }
    const uintptr_t entries = reinterpret_cast<uintptr_t>(m_instance) + m_entriesOffset;
    return ReadValue<uint32_t>(entries + CollectionLayout::kEntryCountOffset);
}

std::optional<std::string> RawStreamerInterface::GetEntryPath(uint16_t entryIndex) const
{
    const std::optional<uint32_t> count = GetEntryCount();
    if (!count || entryIndex >= *count)
    {
        return std::nullopt;
    }

    const uintptr_t entries = reinterpret_cast<uintptr_t>(m_instance) + m_entriesOffset;
    const std::optional<uintptr_t> chunk = ReadValue<uintptr_t>(
        entries + (entryIndex / CollectionLayout::kEntriesPerChunk) * sizeof(uintptr_t));
    if (!chunk)
    {
        return std::nullopt;
    }
    const std::optional<RawCollectionEntryView> entry = ReadValue<RawCollectionEntryView>(
        *chunk +
        (entryIndex % CollectionLayout::kEntriesPerChunk) * sizeof(RawCollectionEntryView));
    if (!entry)
    {
        return std::nullopt;
    }
    return ReadName(reinterpret_cast<uintptr_t>(entry->fileName));
}

std::optional<uint16_t> RawStreamerInterface::GetEntryByName(std::string_view vfsPath) const
{
    if (m_instance == nullptr || HasFaulted())
    {
        return std::nullopt;
    }

    const std::string terminated(vfsPath);
    const std::optional<uint16_t> index = SafeCall(
        "rage::pgRawStreamer::GetEntryByName",
        [&]
        {
            const auto getEntryByName = GetVirtualFunction<GetEntryByNameFn>(
                reinterpret_cast<uintptr_t>(m_instance), CollectionLayout::kSlotGetEntryByName);
            return getEntryByName(m_instance, terminated.c_str());
        });
    if (!index || *index == CollectionLayout::kInvalidEntryIndexRaw)
    {
        return std::nullopt;
    }
    return index;
}

std::optional<std::string> RawStreamerInterface::GetEntryNameFromGame(uint16_t entryIndex) const
{
    if (m_instance == nullptr || HasFaulted())
    {
        return std::nullopt;
    }
    const std::optional<const char*> raw =
        SafeCall("rage::pgRawStreamer::GetEntryName",
                 [&]
                 {
                     const auto getEntryName =
                         GetVirtualFunction<GetEntryNameFn>(reinterpret_cast<uintptr_t>(m_instance),
                                                            CollectionLayout::kSlotGetEntryName);
                     return getEntryName(m_instance, entryIndex);
                 });
    if (!raw || *raw == nullptr)
    {
        return std::nullopt;
    }
    return ReadName(reinterpret_cast<uintptr_t>(*raw));
}
} // namespace spl::rage
