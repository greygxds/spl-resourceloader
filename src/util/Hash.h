#pragma once

#include <cstdint>
#include <string_view>

namespace spl::util
{
namespace detail
{
[[nodiscard]] constexpr uint32_t JoaatStep(uint32_t hash, uint8_t byte)
{
    hash += byte;
    hash += hash << 10;
    hash ^= hash >> 6;
    return hash;
}

[[nodiscard]] constexpr uint32_t JoaatFinish(uint32_t hash)
{
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}
} // namespace detail

/// Jenkins one-at-a-time over the bytes as they are: RAGE's atStringHash without lower-casing.
/// The game hashes data-file type names this way, upper case (FiveM HashRageString).
[[nodiscard]] constexpr uint32_t JoaatExact(std::string_view text)
{
    uint32_t hash = 0;
    for (const char character : text)
    {
        hash = detail::JoaatStep(hash, static_cast<uint8_t>(character));
    }
    return detail::JoaatFinish(hash);
}

/// The same hash over the ASCII lower-cased text: what GET_HASH_KEY gives for a model name.
[[nodiscard]] constexpr uint32_t JoaatLower(std::string_view text)
{
    uint32_t hash = 0;
    for (const char character : text)
    {
        const char lowered = character >= 'A' && character <= 'Z'
                                 ? static_cast<char>(character - 'A' + 'a')
                                 : character;
        hash = detail::JoaatStep(hash, static_cast<uint8_t>(lowered));
    }
    return detail::JoaatFinish(hash);
}
} // namespace spl::util
