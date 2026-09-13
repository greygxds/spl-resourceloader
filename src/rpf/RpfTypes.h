#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace spl::rpf
{
/// RPF7 header, 16 bytes at the start of the file. There is no version field: identity is
/// magic + encryption. FiveM vfs-core/src/VFSRagePackfile7.cpp:28-34, 63-76.
struct RpfHeaderView
{
    uint32_t magic;      // +0x00, 0x52504637 ("RPF7")
    uint32_t entryCount; // +0x04, number of 16-byte entries following the header
    uint32_t nameLength; // +0x08, bytes of the NUL-joined name table after the entries
    uint32_t encryption; // +0x0C, 0x4E45504F ("OPEN") or 0x50584643 ("CFXP")
};
static_assert(sizeof(RpfHeaderView) == 16);

/// One RPF7 entry, 16 bytes. Directories and files share the layout; which fields mean
/// what depends on the offset sentinel. FiveM VFSRagePackfile7.h:18-26.
struct RpfEntryView
{
    uint64_t packed;    // +0x00, nameOffset:16 | size:24 | offset:24
    uint32_t virtFlags; // +0x08, file: decompressed size; directory: first child index
    uint32_t physFlags; // +0x0C, file: resource flags; directory: child count
};
static_assert(sizeof(RpfEntryView) == 16);

namespace RpfLayout
{
constexpr uint32_t kMagicRpf7 = 0x52504637;      ///< "RPF7", little-endian
constexpr uint32_t kEncryptionOpen = 0x4E45504F; ///< "OPEN": accepted as-is
constexpr uint32_t kEncryptionSigned =
    0x50584643; ///< "CFXP": plain, with a signature we do not check

/// offset == this (low 23 bits set) marks a directory (FiveM VFSRagePackfile7.cpp:157).
constexpr uint32_t kDirectorySentinel = 0x7FFFFF;

/// Bit 23 of the offset field: when set, the entry is stored verbatim. Verified against
/// real OpenIV packages 2026-09-13: every flag-set entry starts with plaintext (usually
/// "RSC7"), every flag-clear entry with size != 0 is raw deflate. (FiveM's VFS reader
/// keys off size == 0 instead, which only holds for the archives its own tools write.)
constexpr uint32_t kStoredFlag = 0x800000;

/// File data lives at (offset & sentinel) * sector size, absolute from file start.
constexpr uint64_t kSectorSizeBytes = 512;

/// A resource whose on-disk size does not fit the 24-bit size field says so with all bits set;
/// its first 16 bytes then hold the size instead of the RSC7 header (CodeWalker RpfFile.cs,
/// RpfResourceFileEntry, verified against VDE packages 2026-09-13).
constexpr uint32_t kLargeResourceSize = 0xFFFFFF;

/// The RSC7 magic, "RSC7" little-endian.
constexpr uint32_t kRscMagic = 0x37435352;

/// Sanity caps against corrupt counts: a million entries is 16 MiB of table.
constexpr uint64_t kMaxEntryCount = 1 << 20;
constexpr uint64_t kMaxNameLength = 1 << 26;
constexpr std::size_t kHeaderSizeBytes = sizeof(RpfHeaderView);
constexpr std::size_t kEntrySizeBytes = sizeof(RpfEntryView);
} // namespace RpfLayout

/// Byte offset of the entry's name inside the name table.
[[nodiscard]] constexpr uint16_t RpfNameOffset(const RpfEntryView& entry)
{
    return static_cast<uint16_t>(entry.packed & 0xFFFF);
}

/// On-disk (compressed) size. Zero means stored, not compressed.
[[nodiscard]] constexpr uint32_t RpfDataSize(const RpfEntryView& entry)
{
    return static_cast<uint32_t>((entry.packed >> 16) & 0xFFFFFF);
}

/// Raw 24-bit offset field: sector index plus flags.
[[nodiscard]] constexpr uint32_t RpfOffsetField(const RpfEntryView& entry)
{
    return static_cast<uint32_t>((entry.packed >> 40) & 0xFFFFFF);
}

/// A directory exactly when the low 23 bits of the offset field are all set.
[[nodiscard]] constexpr bool RpfIsDirectory(const RpfEntryView& entry)
{
    return (RpfOffsetField(entry) & RpfLayout::kDirectorySentinel) == RpfLayout::kDirectorySentinel;
}

/// True when the entry's bytes are stored verbatim: the flag bit says so, or the size
/// field is zero (an archive its own tools wrote, e.g. a stored assembly.xml whose
/// length is virtFlags).
[[nodiscard]] constexpr bool RpfIsStored(const RpfEntryView& entry)
{
    return (RpfOffsetField(entry) & RpfLayout::kStoredFlag) != 0 || RpfDataSize(entry) == 0;
}

/// Bytes ReadFile returns for a stored entry: the on-disk size, or virtFlags when the
/// size field is zero.
[[nodiscard]] constexpr uint32_t RpfStoredSize(const RpfEntryView& entry)
{
    return RpfDataSize(entry) != 0 ? RpfDataSize(entry) : entry.virtFlags;
}
/// A resource too large for the size field: its real size is in its data.
[[nodiscard]] constexpr bool RpfIsLargeResource(const RpfEntryView& entry)
{
    return (RpfOffsetField(entry) & RpfLayout::kStoredFlag) != 0 &&
           RpfDataSize(entry) == RpfLayout::kLargeResourceSize;
}

/// The on-disk size of a large resource, from the first 16 bytes of its data.
[[nodiscard]] constexpr uint32_t
RpfLargeResourceSize(std::span<const char, RpfLayout::kHeaderSizeBytes> header)
{
    const auto at = [&header](std::size_t index)
    { return static_cast<uint32_t>(static_cast<uint8_t>(header[index])); };
    return at(7) | (at(14) << 8) | (at(5) << 16) | (at(2) << 24);
}

/// The RSC7 header a resource with these flags starts with. The version is the top nibble of
/// each flag word (verified against the sized entries of the same archives).
[[nodiscard]] constexpr std::array<char, RpfLayout::kHeaderSizeBytes>
MakeRscHeader(uint32_t virtualFlags, uint32_t physicalFlags)
{
    const uint32_t version = ((virtualFlags >> 28) << 4) | (physicalFlags >> 28);
    const std::array<uint32_t, 4> words{RpfLayout::kRscMagic, version, virtualFlags, physicalFlags};
    std::array<char, RpfLayout::kHeaderSizeBytes> header{};
    for (std::size_t word = 0; word < words.size(); ++word)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            header[word * 4 + byte] = static_cast<char>((words[word] >> (byte * 8)) & 0xFF);
        }
    }
    return header;
}
} // namespace spl::rpf
