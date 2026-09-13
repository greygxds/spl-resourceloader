#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "memory/Module.h"
#include "memory/Pattern.h"

namespace spl::memory
{
/// Enough for every signature we use; a pattern matching more often than this is a bad
/// pattern, and the resolver reports it as such instead of scanning the rest of the image.
constexpr size_t kDefaultMaxMatches = 64;

/// Every address in region where pattern matches, lowest first. regionBase is the address
/// region.data() lives at, so the results are absolute addresses. Pure over the bytes, which
/// is what makes it testable with synthetic buffers.
[[nodiscard]] std::vector<uintptr_t> FindAll(const Pattern& pattern,
                                             std::span<const std::byte> region,
                                             uintptr_t regionBase,
                                             size_t maxMatches = kDefaultMaxMatches);

/// Scans one kind of section of a module, caching results for the session. One scanner is
/// created per resolve pass, so the cache never outlives the module it describes.
class PatternScanner
{
public:
    explicit PatternScanner(const Module& module, SectionKind kind = SectionKind::Code);

    /// The matches for pattern, scanned once and then served from the cache. The span stays
    /// valid until ClearCache() or the scanner is destroyed.
    [[nodiscard]] std::span<const uintptr_t> Scan(const Pattern& pattern,
                                                  size_t maxMatches = kDefaultMaxMatches);

    void ClearCache();

    [[nodiscard]] size_t GetCachedPatternCount() const
    {
        return m_cache.size();
    }

private:
    const Module* m_module;
    std::vector<Section> m_regions;
    std::unordered_map<std::string, std::vector<uintptr_t>> m_cache;
};
} // namespace spl::memory
