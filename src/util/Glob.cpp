#include "util/Glob.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/FileTree.h"
#include "util/Strings.h"

namespace spl::util
{
namespace
{
std::vector<std::string> SplitSegments(std::string_view text)
{
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t slash = text.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? text.size() : slash;
        std::string segment{text.substr(start, end - start)};
        if (!segment.empty() && segment != ".")
        {
            segments.push_back(std::move(segment));
        }
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

/// Matches one already-lower-cased segment against one lower-cased pattern segment.
/// Iterative backtracking, so a pathological pattern cannot blow the stack.
bool MatchSegment(std::string_view pattern, std::string_view text)
{
    std::size_t patternIndex = 0;
    std::size_t textIndex = 0;
    std::size_t starPattern = std::string_view::npos;
    std::size_t starText = 0;

    while (textIndex < text.size())
    {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == text[textIndex]))
        {
            ++patternIndex;
            ++textIndex;
        }
        else if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        {
            starPattern = patternIndex++;
            starText = textIndex;
        }
        else if (starPattern != std::string_view::npos)
        {
            patternIndex = starPattern + 1;
            textIndex = ++starText;
        }
        else
        {
            return false;
        }
    }

    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
    {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

/// Matches the remaining pattern segments against the remaining path segments, where a "**"
/// pattern segment may consume any number of path segments.
bool MatchSegments(std::span<const std::string> pattern, std::span<const std::string> path)
{
    if (pattern.empty())
    {
        return path.empty();
    }

    if (pattern.front() == "**")
    {
        for (std::size_t consumed = 0; consumed <= path.size(); ++consumed)
        {
            if (MatchSegments(pattern.subspan(1), path.subspan(consumed)))
            {
                return true;
            }
        }
        return false;
    }

    if (path.empty() || !MatchSegment(pattern.front(), path.front()))
    {
        return false;
    }
    return MatchSegments(pattern.subspan(1), path.subspan(1));
}
} // namespace

bool MatchesGlob(std::string_view pattern, std::string_view path)
{
    const std::string loweredPattern = ToLower(pattern);
    const std::string loweredPath = ToLower(path);
    return MatchSegments(SplitSegments(loweredPattern), SplitSegments(loweredPath));
}

bool IsLiteralPattern(std::string_view pattern)
{
    return pattern.find_first_of("*?") == std::string_view::npos;
}

std::vector<std::string> GlobFiles(const std::filesystem::path& root, std::string_view pattern)
{
    return GlobFiles(DiskFileTree::Instance(), root, pattern);
}

std::vector<std::string> GlobFiles(const IFileTree& files, const std::filesystem::path& root,
                                   std::string_view pattern)
{
    if (pattern.empty())
    {
        return {};
    }

    // A set, so results are sorted and deduplicated the way FiveM's GlobValue produces them.
    std::set<std::string> matches;
    for (std::string& relative : files.ListFilesRecursive(root))
    {
        if (MatchesGlob(pattern, relative))
        {
            matches.insert(std::move(relative));
        }
    }
    return {matches.begin(), matches.end()};
}
} // namespace spl::util
