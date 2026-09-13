#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "resource/Resource.h"

namespace spl::resource
{
/// Walks the resources root and reports the resource folders it finds.
///
/// Pure filesystem work: no configuration is applied here, and nothing is filtered except what
/// cannot be a resource at all. ResourceManager turns the result into Resource objects.
class ResourceScanner
{
public:
    struct Result
    {
        /// In discovery order: breadth-first, each level sorted case-insensitively, so the
        /// outcome never depends on the order NTFS happens to enumerate in.
        std::vector<ResourceCandidate> resources;

        /// Problems worth the user's attention: duplicates, odd names, unreadable folders.
        std::vector<std::string> warnings;
    };

    [[nodiscard]] static Result Scan(const std::filesystem::path& root, bool acceptLegacyManifest);
};

/// True for a folder name of the form "[maps]": a category, never a resource itself.
[[nodiscard]] bool IsCategoryName(std::string_view name);

/// True when every character is in [A-Za-z0-9_-.]. Other names still load, but they map
/// awkwardly onto RAGE paths, so they are worth a warning.
[[nodiscard]] bool IsPlainResourceName(std::string_view name);
} // namespace spl::resource
