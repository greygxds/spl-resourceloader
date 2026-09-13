#include <catch_amalgamated.hpp>

#include "util/Hash.h"

using namespace spl::util;

// Vectors computed independently with a Python one-at-a-time implementation.

TEST_CASE("Hash: JoaatLower matches GET_HASH_KEY for a known model", "[util]")
{
    STATIC_CHECK(JoaatLower("adder") == 0xB779A091);
    CHECK(JoaatLower("ADDER") == 0xB779A091);
    CHECK(JoaatLower("Adder") == 0xB779A091);
    CHECK(JoaatLower("prop_bench_01a") == 0x6BA514AC);
}

TEST_CASE("Hash: JoaatExact keeps the case", "[util]")
{
    CHECK(JoaatExact("adder") == 0xB779A091);
    CHECK(JoaatExact("ADDER") == 0xAFC55086);
    CHECK(JoaatExact("Adder") == 0x2153F8FD);
}

TEST_CASE("Hash: data-file type names hash the way the game's type table stores them", "[util]")
{
    // RPF_FILE is the first row of the table, which is what the "61 44 DF 04" pattern matches.
    CHECK(JoaatExact("RPF_FILE") == 0x04DF4461);
    CHECK(JoaatExact("DLC_ITYP_REQUEST") == 0xF53935BE);
    CHECK(JoaatExact("TEXTFILE_METAFILE") == 0x44180923);
    CHECK(JoaatLower("DLC_ITYP_REQUEST") == 0x8F0FD912);
}

TEST_CASE("Hash: short inputs", "[util]")
{
    CHECK(JoaatExact("") == 0);
    CHECK(JoaatLower("") == 0);
    CHECK(JoaatExact("a") == 0xCA2E9442);
}
