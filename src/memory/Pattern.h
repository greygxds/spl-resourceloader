#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spl::memory
{
/// An IDA-style byte pattern: "48 8B 05 ? ? ? ? 48 8B CB". '?' and '??' are wildcards and
/// hex is case-insensitive. A pattern is immutable once parsed.
class Pattern
{
public:
    /// The longest run of non-wildcard bytes, which the scanner searches for first.
    struct Anchor
    {
        size_t offset = 0;
        size_t lengthBytes = 0;
    };

    /// std::nullopt for malformed text, an empty pattern, or a pattern made only of
    /// wildcards (which would match everywhere).
    [[nodiscard]] static std::optional<Pattern> Parse(std::string_view text);

    /// The literal bytes. Positions covered by a wildcard hold 0 and must not be compared.
    [[nodiscard]] std::span<const uint8_t> GetBytes() const
    {
        return m_bytes;
    }

    /// 0xFF where the byte must match, 0x00 where it is a wildcard.
    [[nodiscard]] std::span<const uint8_t> GetMask() const
    {
        return m_mask;
    }

    [[nodiscard]] size_t GetLengthBytes() const
    {
        return m_bytes.size();
    }

    [[nodiscard]] const Anchor& GetAnchor() const
    {
        return m_anchor;
    }

    /// The text it was parsed from, used as the cache key and in log messages.
    [[nodiscard]] const std::string& GetText() const
    {
        return m_text;
    }

    /// True when the bytes at data match, which the caller must have length-checked.
    [[nodiscard]] bool MatchesAt(const uint8_t* data) const;

private:
    Pattern() = default;

    std::vector<uint8_t> m_bytes;
    std::vector<uint8_t> m_mask;
    std::string m_text;
    Anchor m_anchor;
};
} // namespace spl::memory
