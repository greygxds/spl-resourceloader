#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "memory/Module.h"
#include "memory/Pattern.h"
#include "memory/PatternScanner.h"

using namespace spl::memory;

namespace
{
constexpr uintptr_t kFakeBase = 0x140000000;

[[nodiscard]] std::span<const std::byte> AsBytes(const std::vector<uint8_t>& buffer)
{
    return {reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
}

[[nodiscard]] std::vector<uintptr_t> Find(std::string_view text, const std::vector<uint8_t>& buffer,
                                          size_t maxMatches = kDefaultMaxMatches)
{
    const std::optional<Pattern> pattern = Pattern::Parse(text);
    REQUIRE(pattern.has_value());
    return FindAll(*pattern, AsBytes(buffer), kFakeBase, maxMatches);
}
} // namespace

TEST_CASE("PatternScanner: finds a match and reports its absolute address", "[memory]")
{
    const std::vector<uint8_t> buffer = {0x00, 0x11, 0x48, 0x8B, 0x05, 0x22, 0x33};

    const std::vector<uintptr_t> matches = Find("48 8B 05", buffer);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0] == kFakeBase + 2);
}

TEST_CASE("PatternScanner: wildcards match any byte", "[memory]")
{
    const std::vector<uint8_t> buffer = {0x48, 0x8B, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0x33, 0xC0};

    CHECK(Find("48 8B 05 ? ? ? ? 33 C0", buffer).size() == 1);
    CHECK(Find("48 8B 05 ? ? ? ? 33 C1", buffer).empty());
}

TEST_CASE("PatternScanner: reports every match, lowest address first", "[memory]")
{
    std::vector<uint8_t> buffer(64, 0x90);
    buffer[4] = 0xE8;
    buffer[9] = 0xE8;
    buffer[40] = 0xE8;

    const std::vector<uintptr_t> matches = Find("E8 90 90", buffer);
    REQUIRE(matches.size() == 3);
    CHECK(matches[0] == kFakeBase + 4);
    CHECK(matches[1] == kFakeBase + 9);
    CHECK(matches[2] == kFakeBase + 40);
}

TEST_CASE("PatternScanner: overlapping matches are all found", "[memory]")
{
    const std::vector<uint8_t> buffer = {0xAA, 0xAA, 0xAA, 0xAA};

    const std::vector<uintptr_t> matches = Find("AA AA", buffer);
    REQUIRE(matches.size() == 3);
    CHECK(matches[0] == kFakeBase);
    CHECK(matches[2] == kFakeBase + 2);
}

TEST_CASE("PatternScanner: a match at the very end of the buffer counts", "[memory]")
{
    const std::vector<uint8_t> buffer = {0x00, 0x00, 0x48, 0x8B};

    const std::vector<uintptr_t> matches = Find("48 8B", buffer);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0] == kFakeBase + 2);
}

TEST_CASE("PatternScanner: a pattern longer than the buffer never matches", "[memory]")
{
    const std::vector<uint8_t> buffer = {0x48, 0x8B};

    CHECK(Find("48 8B 05", buffer).empty());
}

TEST_CASE("PatternScanner: no match yields no addresses", "[memory]")
{
    const std::vector<uint8_t> buffer(32, 0x00);

    CHECK(Find("48 8B 05", buffer).empty());
}

TEST_CASE("PatternScanner: maxMatches stops the scan early", "[memory]")
{
    const std::vector<uint8_t> buffer(64, 0xAA);

    CHECK(Find("AA AA", buffer, 1).size() == 1);
    CHECK(Find("AA AA", buffer, 5).size() == 5);
    CHECK(Find("AA AA", buffer, 0).empty());
}

TEST_CASE("PatternScanner: a trailing wildcard needs the bytes to be there", "[memory]")
{
    const std::vector<uint8_t> buffer = {0x00, 0x48, 0x8B};

    // The pattern is 4 bytes long, and only 2 follow the leading 0x00, so it cannot fit.
    CHECK(Find("48 8B ? ?", buffer).empty());
}

TEST_CASE("PatternScanner: scanning a module finds its own code", "[memory]")
{
    const Module main = Module::Main();
    PatternScanner scanner(main);

    // Every x86-64 function ends in a ret, so this must match somewhere in .text.
    const std::optional<Pattern> ret = Pattern::Parse("C3");
    REQUIRE(ret.has_value());

    const std::span<const uintptr_t> matches = scanner.Scan(*ret, 4);
    REQUIRE_FALSE(matches.empty());
    for (const uintptr_t match : matches)
    {
        CHECK(main.Contains(match));
    }
}

TEST_CASE("PatternScanner: a second scan of the same pattern is cached", "[memory]")
{
    const Module main = Module::Main();
    PatternScanner scanner(main);

    const std::optional<Pattern> pattern = Pattern::Parse("C3");
    REQUIRE(pattern.has_value());

    const std::span<const uintptr_t> first = scanner.Scan(*pattern, 4);
    const std::span<const uintptr_t> second = scanner.Scan(*pattern, 4);
    CHECK(scanner.GetCachedPatternCount() == 1);
    CHECK(first.data() == second.data());

    scanner.ClearCache();
    CHECK(scanner.GetCachedPatternCount() == 0);
}

namespace
{
// Distinctive bytes that only exist in the test executable's read-only data.
constexpr std::array<uint8_t, 12> kDataMarker = {0x5A, 0x3C, 0x91, 0xE7, 0x0D, 0x62,
                                                 0xB4, 0x18, 0xF3, 0x7E, 0x2A, 0xC5};
} // namespace

TEST_CASE("PatternScanner: a data scan finds a table a code scan cannot", "[memory]")
{
#if defined(__SANITIZE_ADDRESS__)
    // ASan puts poisoned redzones between the globals in .rdata, so scanning our own image
    // reads them by design; the scanner itself is covered by the synthetic-buffer tests.
    SKIP("scans the test executable's own data sections, which AddressSanitizer poisons");
#else
    const Module main = Module::Main();
    const std::optional<Pattern> pattern = Pattern::Parse("5A 3C 91 E7 0D 62 B4 18 F3 7E 2A C5");
    REQUIRE(pattern.has_value());

    PatternScanner dataScanner(main, SectionKind::Data);
    const std::span<const uintptr_t> matches = dataScanner.Scan(*pattern);
    REQUIRE_FALSE(matches.empty());
    CHECK(std::ranges::find(matches, reinterpret_cast<uintptr_t>(kDataMarker.data())) !=
          matches.end());

    PatternScanner codeScanner(main, SectionKind::Code);
    CHECK(codeScanner.Scan(*pattern).empty());
#endif
}
