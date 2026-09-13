#include "memory/Pattern.h"

#include <cctype>

namespace spl::memory
{
namespace
{
constexpr uint8_t kFixed = 0xFF;
constexpr uint8_t kWildcard = 0x00;

[[nodiscard]] std::optional<uint8_t> HexDigit(char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F')
    {
        return static_cast<uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] bool IsSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}
} // namespace

std::optional<Pattern> Pattern::Parse(std::string_view text)
{
    Pattern pattern;
    size_t index = 0;
    while (index < text.size())
    {
        if (IsSpace(text[index]))
        {
            ++index;
            continue;
        }

        if (text[index] == '?')
        {
            ++index;
            if (index < text.size() && text[index] == '?')
            {
                ++index; // "??" is the same wildcard as "?"
            }
            pattern.m_bytes.push_back(0);
            pattern.m_mask.push_back(kWildcard);
            continue;
        }

        const std::optional<uint8_t> high = HexDigit(text[index]);
        if (!high)
        {
            return std::nullopt;
        }
        ++index;
        // A single hex digit is accepted as its own value, so "8B 5" means 0x8B 0x05.
        uint8_t value = *high;
        if (index < text.size())
        {
            if (const std::optional<uint8_t> low = HexDigit(text[index]))
            {
                value = static_cast<uint8_t>((value << 4) | *low);
                ++index;
            }
        }
        pattern.m_bytes.push_back(value);
        pattern.m_mask.push_back(kFixed);
    }

    if (pattern.m_bytes.empty())
    {
        return std::nullopt;
    }

    Anchor best;
    Anchor current;
    for (size_t position = 0; position < pattern.m_mask.size(); ++position)
    {
        if (pattern.m_mask[position] == kFixed)
        {
            if (current.lengthBytes == 0)
            {
                current.offset = position;
            }
            ++current.lengthBytes;
            if (current.lengthBytes > best.lengthBytes)
            {
                best = current;
            }
        }
        else
        {
            current = Anchor{};
        }
    }
    if (best.lengthBytes == 0)
    {
        return std::nullopt; // all wildcards: it would match at every offset
    }

    pattern.m_anchor = best;
    pattern.m_text = std::string(text);
    return pattern;
}

bool Pattern::MatchesAt(const uint8_t* data) const
{
    for (size_t index = 0; index < m_bytes.size(); ++index)
    {
        if (m_mask[index] == kFixed && data[index] != m_bytes[index])
        {
            return false;
        }
    }
    return true;
}
} // namespace spl::memory
