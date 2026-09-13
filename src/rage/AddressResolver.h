#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "memory/Module.h"
#include "rage/GameBuild.h"
#include "rage/signatures/SignatureSpec.h"

namespace spl::rage
{
/// What one resolve pass produced. Failures are collected rather than thrown, so the log
/// shows every broken signature at once instead of only the first.
struct ResolvedSignatures
{
    std::unordered_map<std::string, uintptr_t> addresses;
    size_t applicable = 0;      ///< rows that apply to this build
    size_t optionalMissing = 0; ///< optional rows that did not resolve
    std::vector<std::string> failures;
    std::unordered_map<std::string, size_t> matchCounts; ///< raw matches per applicable row
    int64_t durationMs = 0;

    /// std::nullopt when the signature did not resolve or does not apply to this build.
    [[nodiscard]] std::optional<uintptr_t> Find(std::string_view name) const;

    /// True when every required signature resolved. Optional misses do not count.
    [[nodiscard]] bool IsComplete() const
    {
        return failures.empty();
    }
};

/// Turns the signature table into addresses. The only place in the codebase that scans the
/// game image.
class AddressResolver
{
public:
    /// Scans module for every signature that applies to gameBuild. Logs one debug line per
    /// signature, one error per failure and an info summary.
    [[nodiscard]] static ResolvedSignatures ResolveAll(const memory::Module& module,
                                                       const GameBuild& gameBuild);

    /// The same, over an explicit table. Used by tests and by the offline signature check
    /// (spl_sigcheck), which runs against a GTA5.exe on disk.
    [[nodiscard]] static ResolvedSignatures ResolveAll(const memory::Module& module,
                                                       const GameBuild& gameBuild,
                                                       std::span<const SignatureSpec> signatures);
};
} // namespace spl::rage
