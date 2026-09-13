#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <catch_amalgamated.hpp>

#include "memory/Address.h"
#include "memory/CodePatch.h"
#include "platform/Win32.h"

using namespace spl::memory;
using spl::ErrorCode;
using spl::Result;

namespace
{
/// A page of executable memory we own, so a patch test never writes into real code. It is
/// left PAGE_EXECUTE_READ, which is what CodePatch has to deal with in the game.
class ExecutablePage
{
public:
    explicit ExecutablePage(std::span<const uint8_t> code) : m_sizeBytes(0x1000)
    {
        m_memory = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, m_sizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(m_memory != nullptr);
        std::memcpy(m_memory, code.data(), code.size());
        DWORD previous = 0;
        REQUIRE(::VirtualProtect(m_memory, m_sizeBytes, PAGE_EXECUTE_READ, &previous) != 0);
    }

    ExecutablePage(const ExecutablePage&) = delete;
    ExecutablePage& operator=(const ExecutablePage&) = delete;

    ~ExecutablePage()
    {
        if (m_memory != nullptr)
        {
            ::VirtualFree(m_memory, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] uintptr_t GetAddress() const
    {
        return reinterpret_cast<uintptr_t>(m_memory);
    }

    [[nodiscard]] std::vector<uint8_t> Read(size_t count) const
    {
        return std::vector<uint8_t>(m_memory, m_memory + count);
    }

private:
    uint8_t* m_memory = nullptr;
    size_t m_sizeBytes;
};

const std::vector<uint8_t> kOriginal = {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3};
const std::vector<uint8_t> kOneNop = {0x90};
const std::vector<uint8_t> kTwoNops = {0x90, 0x90};
const std::vector<uint8_t> kBreakpoint = {0xCC};
} // namespace

TEST_CASE("CodePatch: Write replaces the bytes and Restore puts them back", "[memory]")
{
    ExecutablePage page(kOriginal);
    const std::vector<uint8_t> replacement = {0x33, 0xC0, 0xC3};

    Result<CodePatch> patch = CodePatch::Write("TestReplacement", page.GetAddress(), replacement);
    REQUIRE(patch.HasValue());
    CHECK(patch.GetValue().IsApplied());
    CHECK(patch.GetValue().GetName() == "TestReplacement");
    CHECK(patch.GetValue().GetSizeBytes() == replacement.size());
    CHECK(page.Read(3) == replacement);

    patch.GetValue().Restore();
    CHECK_FALSE(patch.GetValue().IsApplied());
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("CodePatch: Restore is idempotent", "[memory]")
{
    ExecutablePage page(kOriginal);
    Result<CodePatch> patch = CodePatch::Write("TestIdempotent", page.GetAddress(), kOneNop);
    REQUIRE(patch.HasValue());

    patch.GetValue().Restore();
    patch.GetValue().Restore(); // must not write anything a second time
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("CodePatch: the destructor restores an applied patch", "[memory]")
{
    ExecutablePage page(kOriginal);
    {
        Result<CodePatch> patch = CodePatch::Write("TestScoped", page.GetAddress(), kTwoNops);
        REQUIRE(patch.HasValue());
        CHECK(page.Read(1)[0] == 0x90);
    }
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("CodePatch: moving transfers the applied patch exactly once", "[memory]")
{
    ExecutablePage page(kOriginal);
    {
        Result<CodePatch> patch = CodePatch::Write("TestMoved", page.GetAddress(), kTwoNops);
        REQUIRE(patch.HasValue());

        CodePatch moved = std::move(patch.GetValue());
        CHECK(moved.IsApplied());
        CHECK_FALSE(patch.GetValue().IsApplied()); // the source no longer owns the patch
        CHECK(page.Read(1)[0] == 0x90);
    }
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("CodePatch: Nop fills with 0x90", "[memory]")
{
    ExecutablePage page(kOriginal);

    Result<CodePatch> patch = CodePatch::Nop("TestNop", page.GetAddress(), 4);
    REQUIRE(patch.HasValue());
    CHECK(page.Read(4) == std::vector<uint8_t>{0x90, 0x90, 0x90, 0x90});
    CHECK(page.Read(5)[4] == kOriginal[4]); // nothing past the requested count
}

TEST_CASE("CodePatch: unexpected bytes refuse the patch", "[memory]")
{
    ExecutablePage page(kOriginal);
    const std::vector<uint8_t> expected = {0x48, 0x8B, 0xFF}; // the third byte is wrong
    const std::vector<uint8_t> replacement = {0x33, 0xC0, 0xC3};

    Result<CodePatch> patch =
        CodePatch::Write("TestGuard", page.GetAddress(), replacement, expected);
    REQUIRE_FALSE(patch.HasValue());
    CHECK(patch.GetError().code == ErrorCode::NotFound);
    CHECK(page.Read(kOriginal.size()) == kOriginal); // memory untouched
}

TEST_CASE("CodePatch: matching expected bytes allow the patch", "[memory]")
{
    ExecutablePage page(kOriginal);
    const std::vector<uint8_t> expected = {0x48, 0x8B, 0x05};
    const std::vector<uint8_t> replacement = {0x33, 0xC0, 0xC3};

    Result<CodePatch> patch =
        CodePatch::Write("TestGuardOk", page.GetAddress(), replacement, expected);
    REQUIRE(patch.HasValue());
    CHECK(page.Read(3) == replacement);
}

TEST_CASE("CodePatch: an empty or address-less patch is rejected", "[memory]")
{
    ExecutablePage page(kOriginal);

    CHECK_FALSE(CodePatch::Write("TestNoBytes", page.GetAddress(), {}).HasValue());
    CHECK_FALSE(CodePatch::Write("TestNoAddress", 0, kOneNop).HasValue());
    CHECK_FALSE(CodePatch::Nop("TestZeroNops", page.GetAddress(), 0).HasValue());
    // An expected-bytes span of a different length is a mistake in the signature spec.
    const std::vector<uint8_t> expected = {0x48, 0x8B};
    CHECK_FALSE(
        CodePatch::Write("TestLengthMismatch", page.GetAddress(), kOneNop, expected).HasValue());
}

TEST_CASE("CodePatch: WriteCall points the call at the target", "[memory]")
{
    ExecutablePage page(kOriginal);
    // A target inside the same page is always within rel32 reach, so no trampoline is needed.
    void* target = reinterpret_cast<void*>(page.GetAddress() + 0x100);

    Result<CodePatch> patch = CodePatch::WriteCall("TestCall", page.GetAddress(), target);
    REQUIRE(patch.HasValue());
    CHECK(page.Read(1)[0] == 0xE8);
    CHECK(Address(page.GetAddress()).GetCallTarget().GetValue() ==
          reinterpret_cast<uintptr_t>(target));

    patch.GetValue().Restore();
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("CodePatch: WriteCall without a target is rejected", "[memory]")
{
    ExecutablePage page(kOriginal);

    CHECK_FALSE(CodePatch::WriteCall("TestNoTarget", page.GetAddress(), nullptr).HasValue());
}

TEST_CASE("PatchRegistry: RestoreAll undoes every patch it owns", "[memory]")
{
    ExecutablePage first(kOriginal);
    ExecutablePage second(kOriginal);
    PatchRegistry registry;

    REQUIRE(registry.Apply("TestFirst", first.GetAddress(), kTwoNops).HasValue());
    REQUIRE(registry.Apply("TestSecond", second.GetAddress(), kBreakpoint).HasValue());
    CHECK(registry.GetCount() == 2);
    CHECK(first.Read(1)[0] == 0x90);
    CHECK(second.Read(1)[0] == 0xCC);

    registry.RestoreAll();
    CHECK(registry.GetCount() == 0);
    CHECK(first.Read(kOriginal.size()) == kOriginal);
    CHECK(second.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("PatchRegistry: a refused patch is not taken over", "[memory]")
{
    ExecutablePage page(kOriginal);
    PatchRegistry registry;
    const std::vector<uint8_t> expected = {0xFF, 0xFF};

    const Result<void> applied =
        registry.Apply("TestRefused", page.GetAddress(), kTwoNops, expected);
    CHECK_FALSE(applied.HasValue());
    CHECK(registry.GetCount() == 0);
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}

TEST_CASE("PatchRegistry: the destructor restores what is left", "[memory]")
{
    ExecutablePage page(kOriginal);
    {
        PatchRegistry registry;
        REQUIRE(registry.Apply("TestOwned", page.GetAddress(), kOneNop).HasValue());
        CHECK(page.Read(1)[0] == 0x90);
    }
    CHECK(page.Read(kOriginal.size()) == kOriginal);
}
