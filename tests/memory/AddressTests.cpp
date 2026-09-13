#include <cstdint>
#include <cstring>
#include <vector>

#include <catch_amalgamated.hpp>

#include "memory/Address.h"

using spl::memory::Address;

namespace
{
/// A little instruction buffer, so the rip-relative maths is checked against real bytes at
/// a real address instead of hand-computed numbers.
class CodeBuffer
{
public:
    explicit CodeBuffer(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}

    [[nodiscard]] Address At(size_t offset) const
    {
        return Address(reinterpret_cast<uintptr_t>(m_bytes.data() + offset));
    }

    [[nodiscard]] uintptr_t AddressOf(size_t offset) const
    {
        return reinterpret_cast<uintptr_t>(m_bytes.data() + offset);
    }

private:
    std::vector<uint8_t> m_bytes;
};

/// Appends a 32-bit displacement in little-endian order, the way an assembler would.
void PushDisplacement(std::vector<uint8_t>& bytes, int32_t displacement)
{
    uint8_t raw[sizeof(displacement)] = {};
    std::memcpy(raw, &displacement, sizeof(displacement));
    bytes.insert(bytes.end(), std::begin(raw), std::end(raw));
}
} // namespace

TEST_CASE("Address: Add moves in both directions", "[memory]")
{
    const Address base{0x1000};

    CHECK(base.Add(0x10).GetValue() == 0x1010);
    CHECK(base.Add(-0x10).GetValue() == 0x0FF0);
    CHECK(base.Add(0) == base);
}

TEST_CASE("Address: IsNull only for zero", "[memory]")
{
    CHECK(Address{}.IsNull());
    CHECK(Address{0}.IsNull());
    CHECK_FALSE(Address{1}.IsNull());
}

TEST_CASE("Address: ResolveRip counts the displacement from the instruction end", "[memory]")
{
    // 48 8B 05 <disp32>: "mov rax, [rip+disp32]", 7 bytes long.
    std::vector<uint8_t> bytes = {0x48, 0x8B, 0x05};
    PushDisplacement(bytes, 0x20);
    bytes.resize(64, 0x90);
    const CodeBuffer code(std::move(bytes));

    // Pointing at the disp32 itself: target = disp32 address + 4 + displacement.
    CHECK(code.At(3).ResolveRip().GetValue() == code.AddressOf(3) + 4 + 0x20);
    // Pointing at the instruction: the disp32 is 3 bytes in and the instruction ends at 7.
    CHECK(code.At(0).ResolveRip(3, 7).GetValue() == code.AddressOf(0) + 7 + 0x20);
    // Both spellings describe the same instruction, so they must agree.
    CHECK(code.At(3).ResolveRip() == code.At(0).ResolveRip(3, 7));
}

TEST_CASE("Address: ResolveRip handles a negative displacement", "[memory]")
{
    std::vector<uint8_t> bytes(32, 0x90);
    // 48 8D 0D <disp32>: "lea rcx, [rip+disp32]" at offset 8, pointing backwards.
    bytes[8] = 0x48;
    bytes[9] = 0x8D;
    bytes[10] = 0x0D;
    int32_t displacement = -8;
    std::memcpy(bytes.data() + 11, &displacement, sizeof(displacement));
    const CodeBuffer code(std::move(bytes));

    CHECK(code.At(8).ResolveRip(3, 7).GetValue() == code.AddressOf(8) + 7 - 8);
}

TEST_CASE("Address: GetCallTarget resolves E8 rel32", "[memory]")
{
    std::vector<uint8_t> bytes = {0xE8};
    PushDisplacement(bytes, 0x40);
    bytes.resize(128, 0x90);
    const CodeBuffer code(std::move(bytes));

    // The callee is five bytes (the whole instruction) plus the displacement away.
    CHECK(code.At(0).GetCallTarget().GetValue() == code.AddressOf(0) + 5 + 0x40);
}

TEST_CASE("Address: GetJumpTarget resolves E9 rel32", "[memory]")
{
    std::vector<uint8_t> bytes = {0xE9};
    PushDisplacement(bytes, -0x20);
    bytes.resize(128, 0x90);
    const CodeBuffer code(std::move(bytes));

    CHECK(code.At(0).GetJumpTarget().GetValue() == code.AddressOf(0) + 5 - 0x20);
}

TEST_CASE("Address: As reinterprets the value as a pointer", "[memory]")
{
    const uint32_t value = 0xDEADBEEF;
    const Address address(reinterpret_cast<uintptr_t>(&value));

    CHECK(address.As<const uint32_t*>() == &value);
    CHECK(*address.As<const uint32_t*>() == 0xDEADBEEF);
}

TEST_CASE("Address: addresses compare by value", "[memory]")
{
    CHECK(Address{0x100} == Address{0x100});
    CHECK(Address{0x100} < Address{0x200});
    CHECK(Address{0x200} > Address{0x100});
}
