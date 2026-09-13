#include "core/Version.h"

#include <string>

namespace spl
{
std::string Version::Describe()
{
    return std::string{kText} + " (" + std::string{kCommit} + ")";
}
} // namespace spl
