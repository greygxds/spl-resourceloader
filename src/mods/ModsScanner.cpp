#include "mods/ModsScanner.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mods/AssemblyParser.h"
#include "rpf/RpfReader.h"
#include "util/Strings.h"

namespace spl::mods
{
namespace
{
/// FiveM ModPackage.cpp:154 matches ".rpf" case-sensitively.
[[nodiscard]] bool IsModArchive(const std::filesystem::path& file)
{
    return file.extension().native() == L".rpf";
}
} // namespace

ModsScanner::Result ModsScanner::Scan(const std::filesystem::path& modsRoot)
{
    Result result;
    std::error_code error;
    if (!std::filesystem::is_directory(modsRoot, error))
    {
        return result;
    }

    std::vector<std::filesystem::path> archives;
    for (std::filesystem::directory_iterator entry{modsRoot, error}, end; entry != end && !error;
         entry.increment(error))
    {
        if (!entry->is_regular_file(error) || error)
        {
            continue;
        }
        if (IsModArchive(entry->path()))
        {
            archives.push_back(entry->path());
        }
        else if (util::EqualsIgnoreCase(util::ToUtf8(entry->path().extension()), ".rpf"))
        {
            result.ignoredArchives.push_back(util::ToUtf8(entry->path().filename()));
        }
    }
    if (error)
    {
        result.warnings.push_back("could not list '" + util::ToUtf8(modsRoot) + "'");
        return result;
    }

    std::ranges::sort(archives, {}, [](const std::filesystem::path& file)
                      { return util::ToLower(util::ToUtf8(file.filename())); });

    for (const std::filesystem::path& archive : archives)
    {
        const std::string name = util::ToUtf8(archive.stem());
        spl::Result<rpf::RpfReader> opened = rpf::RpfReader::Open(archive);
        if (!opened)
        {
            result.warnings.push_back("skipping '" + util::ToUtf8(archive.filename()) +
                                      "': " + opened.GetMessage());
            continue;
        }
        const spl::Result<std::vector<std::byte>> assembly =
            opened.GetValue().ReadFile("assembly.xml");
        if (!assembly)
        {
            result.warnings.push_back("skipping '" + util::ToUtf8(archive.filename()) +
                                      "': no assembly.xml (" + assembly.GetMessage() + ")");
            continue;
        }
        const std::string text{reinterpret_cast<const char*>(assembly.GetValue().data()),
                               assembly.GetValue().size()};
        AssemblyResult parsed = AssemblyParser::ParseAssembly(text);
        if (parsed.fatal)
        {
            result.warnings.push_back("skipping '" + util::ToUtf8(archive.filename()) +
                                      "': broken assembly.xml");
            continue;
        }
        if (parsed.package.entries.empty())
        {
            continue; // FiveM ModPackage.cpp:183 discards entry-less packages silently
        }
        result.mods.push_back(DiscoveredMod{
            .name = name, .absolutePath = archive, .package = std::move(parsed.package)});
    }
    return result;
}

ModsScanner::Selection ModsScanner::Select(std::vector<DiscoveredMod>& mods,
                                           const config::ModsSettings& settings)
{
    const auto indexIn = [](const std::vector<std::string>& names, std::string_view name)
    {
        const auto match =
            std::ranges::find_if(names, [&](const std::string& candidate)
                                 { return util::EqualsIgnoreCase(candidate, name); });
        return static_cast<std::size_t>(match - names.begin());
    };

    Selection selection;
    std::erase_if(mods,
                  [&](const DiscoveredMod& mod)
                  {
                      const bool disabled =
                          indexIn(settings.disabled, mod.name) < settings.disabled.size();
                      if (disabled)
                      {
                          selection.disabled.push_back(mod.name);
                      }
                      return disabled;
                  });

    for (const std::string& wanted : settings.priority)
    {
        const bool present =
            std::ranges::any_of(mods, [&](const DiscoveredMod& mod)
                                { return util::EqualsIgnoreCase(mod.name, wanted); });
        if (!present)
        {
            selection.missingPriority.push_back(wanted);
        }
    }

    // Unlisted mods get priority.size(), so they sort after every listed one, in Scan's order.
    std::ranges::stable_sort(mods, {}, [&](const DiscoveredMod& mod)
                             { return indexIn(settings.priority, mod.name); });
    return selection;
}
} // namespace spl::mods
