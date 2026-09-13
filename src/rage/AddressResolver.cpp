#include "rage/AddressResolver.h"

#include <algorithm>
#include <chrono>

#include <spdlog/fmt/fmt.h>

#include "logging/Logger.h"
#include "memory/Address.h"
#include "memory/Pattern.h"
#include "memory/PatternScanner.h"

namespace spl::rage
{
namespace
{
/// Addresses are always logged as module + offset, never absolute: the image base changes
/// every launch, the offset does not (conventions section 8).
[[nodiscard]] std::string FormatAddress(const memory::Module& module, uintptr_t address)
{
    if (!module.Contains(address))
    {
        return fmt::format("{:#x} (outside {})", address, module.GetFileName());
    }
    return fmt::format("{}+{:#x}", module.GetFileName(), address - module.GetBase());
}

[[nodiscard]] bool AppliesTo(const SignatureSpec& spec, const GameBuild& gameBuild)
{
    return gameBuild.build >= spec.minBuild && gameBuild.build <= spec.maxBuild;
}

/// Applies the spec's offset and resolve kind to a raw match.
[[nodiscard]] uintptr_t ResolveMatch(const memory::Module& module, const SignatureSpec& spec,
                                     uintptr_t match)
{
    const memory::Address base = memory::Address(match).Add(spec.offset);
    using enum ResolveKind;
    switch (spec.kind)
    {
    case Direct:
        return base.GetValue();
    case CallTarget:
        return base.GetCallTarget().GetValue();
    case JumpTarget:
        return base.GetJumpTarget().GetValue();
    case RipRelative:
        return base.ResolveRip(spec.ripDispOffset, spec.ripInstrEnd).GetValue();
    case RvaFromImageBase:
        return module.GetBase() + *base.As<const uint32_t*>();
    }
    return base.GetValue();
}

/// Every address the matches resolve to, lowest first, without repeats.
[[nodiscard]] std::vector<uintptr_t> ResolveDistinctTargets(const memory::Module& module,
                                                            const SignatureSpec& spec,
                                                            std::span<const uintptr_t> matches)
{
    std::vector<uintptr_t> targets;
    targets.reserve(matches.size());
    for (const uintptr_t match : matches)
    {
        targets.push_back(ResolveMatch(module, spec, match));
    }
    std::ranges::sort(targets);
    const auto [first, last] = std::ranges::unique(targets);
    targets.erase(first, last);
    return targets;
}

/// "GTA5.exe+0x1234, GTA5.exe+0x5678", capped so a runaway pattern keeps the log line short.
[[nodiscard]] std::string DescribeTargets(const memory::Module& module,
                                          std::span<const uintptr_t> targets)
{
    constexpr std::size_t kMaxListed = 4;
    std::string description;
    for (std::size_t index = 0; index < targets.size() && index < kMaxListed; ++index)
    {
        if (!description.empty())
        {
            description += ", ";
        }
        description += FormatAddress(module, targets[index]);
    }
    if (targets.size() > kMaxListed)
    {
        description += ", ...";
    }
    return description;
}
} // namespace

std::optional<uintptr_t> ResolvedSignatures::Find(std::string_view name) const
{
    const auto match = addresses.find(std::string(name));
    if (match == addresses.end())
    {
        return std::nullopt;
    }
    return match->second;
}

ResolvedSignatures AddressResolver::ResolveAll(const memory::Module& module,
                                               const GameBuild& gameBuild)
{
    return ResolveAll(module, gameBuild, AllSignatures());
}

ResolvedSignatures AddressResolver::ResolveAll(const memory::Module& module,
                                               const GameBuild& gameBuild,
                                               std::span<const SignatureSpec> signatures)
{
    const auto started = std::chrono::steady_clock::now();
    ResolvedSignatures resolved;
    memory::PatternScanner codeScanner(module, memory::SectionKind::Code);
    memory::PatternScanner dataScanner(module, memory::SectionKind::Data);
    memory::PatternScanner anyScanner(module, memory::SectionKind::Any);
    const auto scannerFor = [&](memory::SectionKind kind) -> memory::PatternScanner&
    {
        using enum memory::SectionKind;
        switch (kind)
        {
        case Code:
            return codeScanner;
        case Data:
            return dataScanner;
        case Any:
            return anyScanner;
        }
        return codeScanner;
    };

    for (const SignatureSpec& spec : signatures)
    {
        if (!AppliesTo(spec, gameBuild))
        {
            SPL_LOG_DEBUG(Rage, "Signature '{}' does not apply to build {}", spec.name,
                          gameBuild.build);
            continue;
        }
        ++resolved.applicable;

        const std::optional<memory::Pattern> pattern = memory::Pattern::Parse(spec.pattern);
        if (!pattern)
        {
            resolved.failures.push_back(fmt::format("signature '{}' has a malformed pattern '{}'",
                                                    spec.name, spec.pattern));
            SPL_LOG_ERROR(Rage, resolved.failures.back());
            continue;
        }

        const std::span<const uintptr_t> matches = scannerFor(spec.section).Scan(*pattern);
        resolved.matchCounts[std::string(spec.name)] = matches.size();

        std::optional<uintptr_t> resolvedAddress;
        std::string reason;
        if (matches.empty() || matches.size() <= spec.matchIndex)
        {
            reason = matches.empty() ? "no match"
                                     : fmt::format("pattern matched {} times, fewer than match {}",
                                                   matches.size(), spec.matchIndex + 1);
        }
        else if (spec.expectedMatches == 0 || matches.size() == spec.expectedMatches)
        {
            resolvedAddress = ResolveMatch(module, spec, matches[spec.matchIndex]);
        }
        else
        {
            // FiveM's get_pattern stops at the first match and never sees the others. Extra
            // matches are harmless when they all resolve to one address (several references to
            // the same object); when they disagree, the first one is a guess we do not make.
            const std::vector<uintptr_t> targets = ResolveDistinctTargets(module, spec, matches);
            if (targets.size() == 1)
            {
                resolvedAddress = targets.front();
            }
            else
            {
                reason = fmt::format("pattern matched {} times, expected {}, and the matches "
                                     "resolve to {} different addresses ({})",
                                     matches.size(), spec.expectedMatches, targets.size(),
                                     DescribeTargets(module, targets));
            }
        }

        if (!resolvedAddress)
        {
            if (!spec.required)
            {
                ++resolved.optionalMissing;
                SPL_LOG_DEBUG(Rage, "Optional signature '{}' unresolved ({}) for pattern '{}'",
                              spec.name, reason, spec.pattern);
                continue;
            }
            resolved.failures.push_back(fmt::format("signature '{}': {} for pattern '{}'",
                                                    spec.name, reason, spec.pattern));
            SPL_LOG_ERROR(Rage, resolved.failures.back());
            continue;
        }

        const uintptr_t address = *resolvedAddress;
        if (spec.kind != ResolveKind::Direct && !module.Contains(address))
        {
            resolved.failures.push_back(
                fmt::format("signature '{}' resolved to {} which is outside the module", spec.name,
                            FormatAddress(module, address)));
            SPL_LOG_ERROR(Rage, resolved.failures.back());
            continue;
        }

        resolved.addresses.emplace(std::string(spec.name), address);
        SPL_LOG_DEBUG(Rage, "{} -> {} ({} match(es), {})", spec.name,
                      FormatAddress(module, address), matches.size(), ToString(spec.kind));
    }

    resolved.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    SPL_LOG_DEBUG(Rage, "Resolved {}/{} signatures ({} optional missing) in {} ms",
                  resolved.addresses.size(), resolved.applicable, resolved.optionalMissing,
                  resolved.durationMs);
    return resolved;
}
} // namespace spl::rage
