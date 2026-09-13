#pragma once

#include <compare>
#include <cstdint>

namespace spl::memory
{
/// A code or data address, with the pointer arithmetic that reading x86-64 instructions
/// needs. Reading through one only makes sense for an address inside a loaded module.
class Address
{
public:
    constexpr Address() = default;

    constexpr explicit Address(uintptr_t value) : m_value(value) {}

    [[nodiscard]] constexpr uintptr_t GetValue() const
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsNull() const
    {
        return m_value == 0;
    }

    [[nodiscard]] constexpr Address Add(ptrdiff_t offset) const
    {
        return Address(static_cast<uintptr_t>(static_cast<ptrdiff_t>(m_value) + offset));
    }

    /// The target of a rip-relative operand. dispOffset is where the disp32 sits relative to
    /// this address, and instructionEnd how far the end of the instruction is from it; the
    /// displacement counts from the end. The defaults describe an address that already points
    /// at the disp32, so "48 8B 05 <disp32>" resolves as Add(3).ResolveRip().
    ///
    /// This matches FiveM's hook::get_address: ResolveRip() is get_address<T>(p) and
    /// ResolveRip(3, 7) is get_address(p, 3, 7).
    [[nodiscard]] Address ResolveRip(ptrdiff_t dispOffset = 0, ptrdiff_t instructionEnd = 4) const
    {
        const int32_t displacement = *Add(dispOffset).As<const int32_t*>();
        return Add(instructionEnd + displacement);
    }

    /// The callee of the "E8 rel32" at this address. The opcode byte is not checked, because
    /// a signature already established what this instruction is.
    [[nodiscard]] Address GetCallTarget() const
    {
        return Add(1).ResolveRip();
    }

    /// The destination of the "E9 rel32" at this address.
    [[nodiscard]] Address GetJumpTarget() const
    {
        return Add(1).ResolveRip();
    }

    template <typename T> [[nodiscard]] T As() const
    {
        return reinterpret_cast<T>(m_value);
    }

    [[nodiscard]] friend constexpr bool operator==(Address left, Address right) = default;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Address left,
                                                                    Address right) = default;

private:
    uintptr_t m_value = 0;
};
} // namespace spl::memory
