#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "memory/Module.h"

namespace spl::rage
{
/// What to do with a pattern match to get the address we actually want.
enum class ResolveKind
{
    Direct,          ///< the match itself
    CallTarget,      ///< "E8 rel32" at the match: the callee
    JumpTarget,      ///< "E9 rel32" at the match: the destination
    RipRelative,     ///< a rip-relative operand: the data or function it points at
    RvaFromImageBase ///< a uint32 RVA stored at the match, relative to the image base
};

[[nodiscard]] std::string_view ToString(ResolveKind kind);

/// One row of the signature table. Everything version-sensitive about an address lives here,
/// so a new game build is a data change (conventions section 3).
struct SignatureSpec
{
    std::string_view name;    ///< the RAGE spelling: "strStreamingInfoManager::sm_instance"
    std::string_view pattern; ///< IDA style: "48 8B 05 ? ? ? ? 48 8B CB"
    ptrdiff_t offset = 0;     ///< applied to the match before it is resolved
    ResolveKind kind = ResolveKind::Direct;
    ptrdiff_t ripDispOffset = 0; ///< RipRelative: where the disp32 sits, from (match + offset)
    ptrdiff_t ripInstrEnd = 4;   ///< RipRelative: end of the instruction, from (match + offset)

    /// Where the pattern lives. Data covers tables in .rdata/.data, which a code scan skips.
    memory::SectionKind section = memory::SectionKind::Code;

    /// How many matches the pattern should have. More are accepted only when every match
    /// resolves to the same address. 0 means "at least one", and then matchIndex picks one.
    size_t expectedMatches = 1;
    size_t matchIndex = 0;

    /// A missing required signature fails the resolve pass; a missing optional one is only
    /// counted and logged.
    bool required = true;

    uint32_t minBuild = 0;          ///< first game build this row applies to
    uint32_t maxBuild = UINT32_MAX; ///< last game build this row applies to
    std::string_view source;        ///< where the pattern came from, for traceability
};

/// Every signature the loader knows, in one table. Phases 6 to 9 add their rows here and
/// nowhere else.
[[nodiscard]] std::span<const SignatureSpec> AllSignatures();
} // namespace spl::rage
