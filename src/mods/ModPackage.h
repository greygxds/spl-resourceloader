#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spl::mods
{
/// Display metadata from assembly.xml's <metadata>, straight through.
struct ModMetadata
{
    std::string name;
    std::string version; ///< "major.minor", formatted at parse time
    std::string authorName;
    std::string description;
};

/// One <add> entry: the file to take and where the mod claims it belongs.
struct ModEntry
{
    std::vector<std::string> archiveRoots; ///< enclosing <archive path="...">, outer first
    std::string sourceFile;                ///< <add source="...">, as written
    std::string targetFile;                ///< <add> text, as written
};

/// A parsed assembly.xml. Only the Add shape exists upstream; anything else is ignored
/// at parse time (FiveM ModPackage.cpp:40-72).
struct ModPackage
{
    std::string guid; ///< canonical 8-4-4-4-12 upper-case, or all zeros when malformed
    ModMetadata metadata;
    std::vector<ModEntry> entries;
};

/// setup2.xml: how a DLC-style mod wants its outer archive mounted (wired in M5).
struct DlcDescriptor
{
    std::string deviceName;
    int32_t order = 0;
    std::string requiredVersion; ///< "" means any build; otherwise "min[-max]"
};

/// One content.xml dataFiles/Item (wired in M5).
struct ContentItem
{
    std::string filename;
    std::string fileType;
};
} // namespace spl::mods
