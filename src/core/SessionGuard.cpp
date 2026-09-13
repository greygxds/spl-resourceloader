#include "core/SessionGuard.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <toml++/toml.hpp>

#include "config/LoaderConfig.h"
#include "core/Result.h"
#include "util/Strings.h"

namespace spl
{
namespace
{
constexpr std::string_view kStageKey = "stage=";
constexpr std::string_view kResourceKey = "resource=";
constexpr std::string_view kFileKey = "file=";

/// A marker value is one line; a resource folder name with a line break in it is not worth
/// corrupting the file over.
std::string OneLine(std::string_view text)
{
    std::string line{text};
    std::ranges::replace(line, '\r', ' ');
    std::ranges::replace(line, '\n', ' ');
    return line;
}

bool ContainsIgnoreCase(const std::vector<std::string>& names, std::string_view name)
{
    return std::ranges::any_of(names, [&](const std::string& candidate)
                               { return util::EqualsIgnoreCase(candidate, name); });
}

std::optional<std::string> ReadWholeFile(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::exists(file, error))
    {
        return std::nullopt;
    }
    std::ifstream stream{file, std::ios::binary};
    if (!stream)
    {
        return std::string{}; // present but unreadable still says the session did not finish
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void WriteWholeFile(const std::filesystem::path& file, std::string_view contents)
{
    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    stream << contents;
}
} // namespace

SessionGuard::SessionGuard(std::filesystem::path markerFile, std::filesystem::path stateFile)
    : m_markerFile(std::move(markerFile)), m_stateFile(std::move(stateFile))
{
}

SessionStartup SessionGuard::Begin(config::SafeMode mode)
{
    std::string stateError;
    std::vector<std::string> quarantined;
    if (const std::optional<std::string> state = ReadWholeFile(m_stateFile))
    {
        if (Result<std::vector<std::string>> parsed = ParseState(*state))
        {
            quarantined = std::move(parsed.GetValue());
        }
        else
        {
            stateError = parsed.GetMessage();
        }
    }

    std::optional<CrashMarker> marker;
    if (const std::optional<std::string> text = ReadWholeFile(m_markerFile))
    {
        marker = ParseMarker(*text);
        std::error_code error;
        std::filesystem::remove(m_markerFile, error);
    }

    SessionStartup startup = Decide(std::move(marker), mode, std::move(quarantined));
    startup.stateError = std::move(stateError);
    m_quarantined = startup.quarantined;
    if (!startup.newlyQuarantined.empty())
    {
        WriteWholeFile(m_stateFile, FormatState(m_quarantined));
    }
    m_busy = false;
    return startup;
}

SessionStartup SessionGuard::Decide(std::optional<CrashMarker> marker, config::SafeMode mode,
                                    std::vector<std::string> quarantined)
{
    SessionStartup startup;
    startup.quarantined = std::move(quarantined);
    if (!marker)
    {
        return startup;
    }

    startup.previousCrash = std::move(marker);
    if (mode == config::SafeMode::Off)
    {
        return startup;
    }

    const std::string& culprit = startup.previousCrash->resource;
    if (culprit.empty())
    {
        startup.safeMode = true;
        return startup;
    }
    if (!ContainsIgnoreCase(startup.quarantined, culprit))
    {
        startup.quarantined.push_back(culprit);
        startup.newlyQuarantined = culprit;
    }
    return startup;
}

void SessionGuard::MarkBusy(std::string_view stage)
{
    if (m_busy || m_markerFile.empty())
    {
        return;
    }
    m_busy = true;
    WriteWholeFile(m_markerFile, FormatMarker(CrashMarker{.stage = std::string{stage}}));
}

void SessionGuard::MarkIdle()
{
    if (!m_busy)
    {
        return;
    }
    m_busy = false;
    std::error_code error;
    std::filesystem::remove(m_markerFile, error);
}

void SessionGuard::RecordCrash(const CrashMarker& marker) const
{
    if (m_markerFile.empty())
    {
        return;
    }
    WriteWholeFile(m_markerFile, FormatMarker(marker));
}

void SessionGuard::Quarantine(std::string_view resourceName)
{
    if (resourceName.empty() || ContainsIgnoreCase(m_quarantined, resourceName))
    {
        return;
    }
    m_quarantined.emplace_back(resourceName);
    if (!m_stateFile.empty())
    {
        WriteWholeFile(m_stateFile, FormatState(m_quarantined));
    }
}

std::string SessionGuard::FormatMarker(const CrashMarker& marker)
{
    return fmt::format("{}{}\n{}{}\n{}{}\n", kStageKey, OneLine(marker.stage), kResourceKey,
                       OneLine(marker.resource), kFileKey, OneLine(marker.file));
}

CrashMarker SessionGuard::ParseMarker(std::string_view text)
{
    CrashMarker marker;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        const std::string line = util::Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);

        if (line.starts_with(kStageKey))
        {
            marker.stage = util::Trim(std::string_view{line}.substr(kStageKey.size()));
        }
        else if (line.starts_with(kResourceKey))
        {
            marker.resource = util::Trim(std::string_view{line}.substr(kResourceKey.size()));
        }
        else if (line.starts_with(kFileKey))
        {
            marker.file = util::Trim(std::string_view{line}.substr(kFileKey.size()));
        }
    }
    return marker;
}

std::string SessionGuard::FormatState(const std::vector<std::string>& quarantined)
{
    toml::array names;
    for (const std::string& name : quarantined)
    {
        names.push_back(name);
    }
    toml::table resources;
    resources.insert("quarantined", std::move(names));
    toml::table root;
    root.insert("resources", std::move(resources));

    std::ostringstream text;
    text << "# Written by the loader; config.toml is never changed for you.\n"
            "# A quarantined resource crashed the game while it was being registered and is\n"
            "# skipped. Remove its name to try it again.\n\n"
         << root << '\n';
    return text.str();
}

Result<std::vector<std::string>> SessionGuard::ParseState(std::string_view tomlText)
{
    toml::table root;
    try
    {
        root = toml::parse(tomlText);
    }
    catch (const std::exception& exception)
    {
        return MakeError(ErrorCode::Parse,
                         "state.toml could not be parsed, so no resource is quarantined: {}",
                         exception.what());
    }

    std::vector<std::string> names;
    const toml::array* list = root["resources"]["quarantined"].as_array();
    if (list == nullptr)
    {
        return names;
    }
    for (const toml::node& element : *list)
    {
        const std::optional<std::string> value = element.value<std::string>();
        if (!value)
        {
            continue;
        }
        std::string name = util::Trim(*value);
        if (!name.empty() && !ContainsIgnoreCase(names, name))
        {
            names.push_back(std::move(name));
        }
    }
    return names;
}
} // namespace spl
