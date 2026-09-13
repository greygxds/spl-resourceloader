#include "util/Strings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace spl::util
{
namespace
{
constexpr std::string_view kWhitespace = " \t\r\n\f\v";

char LowerChar(char character)
{
    // Cast through unsigned char: std::tolower has undefined behavior for negative values,
    // which a signed char gives for bytes above 0x7F.
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}
} // namespace

std::string ToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string ToUtf8Generic(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string ToLower(std::string_view text)
{
    std::string lowered;
    lowered.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(lowered), LowerChar);
    return lowered;
}

std::string ToUpper(std::string_view text)
{
    std::string uppered;
    uppered.reserve(text.size());
    std::ranges::transform(
        text, std::back_inserter(uppered), [](char character)
        { return static_cast<char>(std::toupper(static_cast<unsigned char>(character))); });
    return uppered;
}

std::string Trim(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return std::string{text.substr(first, last - first + 1)};
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    return std::ranges::equal(left, right,
                              [](char a, char b) { return LowerChar(a) == LowerChar(b); });
}
} // namespace spl::util
