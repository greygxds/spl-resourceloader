#include "memory/PatternScanner.h"

#include <cstring>

namespace spl::memory
{
std::vector<uintptr_t> FindAll(const Pattern& pattern, std::span<const std::byte> region,
                               uintptr_t regionBase, size_t maxMatches)
{
    std::vector<uintptr_t> matches;
    const size_t lengthBytes = pattern.GetLengthBytes();
    if (maxMatches == 0 || lengthBytes == 0 || region.size() < lengthBytes)
    {
        return matches;
    }

    const Pattern::Anchor& anchor = pattern.GetAnchor();
    const uint8_t* const data = reinterpret_cast<const uint8_t*>(region.data());
    const uint8_t anchorByte = pattern.GetBytes()[anchor.offset];
    const uint8_t* const anchorBytes = pattern.GetBytes().data() + anchor.offset;
    const size_t lastStart = region.size() - lengthBytes; // inclusive

    size_t start = 0;
    while (start <= lastStart)
    {
        // Look for the anchor's first byte, then widen the check outwards. memchr skips the
        // bulk of a ~60 MB image without ever touching the mask.
        const void* found =
            std::memchr(data + start + anchor.offset, anchorByte, lastStart - start + 1);
        if (found == nullptr)
        {
            break;
        }
        start = static_cast<size_t>(static_cast<const uint8_t*>(found) - data) - anchor.offset;

        if (std::memcmp(data + start + anchor.offset, anchorBytes, anchor.lengthBytes) == 0 &&
            pattern.MatchesAt(data + start))
        {
            matches.push_back(regionBase + start);
            if (matches.size() >= maxMatches)
            {
                break;
            }
        }
        ++start;
    }
    return matches;
}

PatternScanner::PatternScanner(const Module& module, SectionKind kind)
    : m_module(&module), m_regions(module.GetScanRegions(kind))
{
}

std::span<const uintptr_t> PatternScanner::Scan(const Pattern& pattern, size_t maxMatches)
{
    const auto cached = m_cache.find(pattern.GetText());
    if (cached != m_cache.end())
    {
        return cached->second;
    }

    std::vector<uintptr_t> matches;
    for (const Section& region : m_regions)
    {
        if (matches.size() >= maxMatches)
        {
            break;
        }
        std::vector<uintptr_t> found =
            FindAll(pattern, m_module->GetBytes(region), region.begin, maxMatches - matches.size());
        matches.insert(matches.end(), found.begin(), found.end());
    }

    return m_cache.emplace(pattern.GetText(), std::move(matches)).first->second;
}

void PatternScanner::ClearCache()
{
    m_cache.clear();
}
} // namespace spl::memory
